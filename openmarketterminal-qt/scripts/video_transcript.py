"""
Video Transcript Fetcher
Fetches captions/subtitles from video URLs (YouTube, Vimeo, etc.)
and returns a cleaned transcript as JSON.

Returns JSON to stdout, exits 0 on both success and handled errors.
"""

import sys
import os
import json
import re
import glob
import tempfile
import subprocess


def _find_ytdlp_binary():
    """Find the yt-dlp binary: prefer /opt/homebrew/bin, then PATH."""
    candidates = ["/opt/homebrew/bin/yt-dlp", "/usr/local/bin/yt-dlp"]
    for p in candidates:
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    # Fall back to PATH
    try:
        result = subprocess.run(
            ["which", "yt-dlp"], capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return None


def _parse_vtt(vtt_text):
    """
    Parse a VTT subtitle file into a clean, deduplicated transcript.

    Strategy:
    - Strip WEBVTT header, Kind/Language lines, timestamp lines (contain -->),
      cue number lines, and inline timing/tag markup like <00:00:00.000> and <c>.
    - Deduplicate consecutive identical lines (YouTube rolling auto-captions
      repeat each line several times as it scrolls).
    - Join into readable paragraphs.
    """
    lines = vtt_text.splitlines()

    cleaned = []
    for line in lines:
        line = line.strip()

        # Skip header and metadata
        if not line:
            continue
        if line.startswith("WEBVTT"):
            continue
        if line.startswith("Kind:") or line.startswith("Language:"):
            continue
        # Skip timestamp lines (contain -->)
        if "-->" in line:
            continue
        # Skip pure cue-number lines (just a number)
        if re.fullmatch(r"\d+", line):
            continue

        # Remove inline timing tags: <00:00:00.000> and <00:00:00.000><c>
        line = re.sub(r"<\d{2}:\d{2}:\d{2}\.\d+>", "", line)
        # Remove <c> and </c> tags and other HTML-like tags
        line = re.sub(r"<[^>]+>", "", line)

        line = line.strip()
        if line:
            cleaned.append(line)

    # Deduplicate consecutive identical lines (YouTube rolling captions)
    deduped = []
    prev = None
    for line in cleaned:
        if line != prev:
            deduped.append(line)
        prev = line

    # Join into paragraphs: group lines into chunks of ~5 sentences
    # separated by double newlines for readability.
    chunks = []
    current_chunk = []
    for line in deduped:
        current_chunk.append(line)
        if len(current_chunk) >= 5:
            chunks.append(" ".join(current_chunk))
            current_chunk = []
    if current_chunk:
        chunks.append(" ".join(current_chunk))

    return "\n\n".join(chunks)


def _fetch_via_module(url, tmpdir):
    """
    Fetch captions using the yt_dlp Python module.
    Returns (title, manual_langs) where manual_langs is the set of language
    codes that have *manual* (non-auto-generated) subtitles.  The caller uses
    this to classify the chosen .vtt file.
    """
    import yt_dlp  # noqa: import inside function (optional dep)

    ydl_opts = {
        "skip_download": True,
        "writesubtitles": True,
        "writeautomaticsub": True,
        "subtitleslangs": ["en.*"],
        "subtitlesformat": "vtt",
        "outtmpl": os.path.join(tmpdir, "%(id)s.%(ext)s"),
        "quiet": True,
        "no_warnings": True,
    }

    with yt_dlp.YoutubeDL(ydl_opts) as ydl:
        info = ydl.extract_info(url, download=True)

    title = info.get("title", "") if info else ""
    # info['subtitles'] holds manual tracks; info['automatic_captions'] holds auto.
    manual_langs = set((info.get("subtitles") or {}).keys()) if info else set()
    return title, manual_langs


def _fetch_via_binary(url, tmpdir):
    """
    Fetch captions using the yt-dlp binary as a subprocess.
    Returns (returncode, stderr_text).
    """
    binary = _find_ytdlp_binary()
    if not binary:
        raise FileNotFoundError("yt-dlp binary not found")

    outtmpl = os.path.join(tmpdir, "%(id)s.%(ext)s")
    cmd = [
        binary,
        "--write-auto-subs",
        "--write-subs",
        "--sub-langs", "en.*",
        "--sub-format", "vtt",
        "--skip-download",
        "--quiet",
        "--no-warnings",
        "-o", outtmpl,
        url,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return result.returncode, result.stderr


def _get_info_via_binary(url):
    """
    Fetch the info dict (JSON) via yt-dlp --dump-json without downloading.
    Returns (title, manual_langs) where manual_langs is the set of language
    codes that have *manual* subtitles.
    Falls back gracefully to ("", set()) on any failure.
    """
    binary = _find_ytdlp_binary()
    if not binary:
        return "", set()
    try:
        result = subprocess.run(
            [binary, "--dump-json", "--skip-download", "--quiet", "--no-warnings", url],
            capture_output=True, text=True, timeout=60
        )
        if result.returncode == 0 and result.stdout.strip():
            info = json.loads(result.stdout.strip().splitlines()[-1])
            title = info.get("title", "")
            manual_langs = set((info.get("subtitles") or {}).keys())
            return title, manual_langs
    except Exception:
        pass
    return "", set()


def _download_audio(url):
    """
    Download audio for `url` and convert to 16 kHz mono WAV.
    Returns the absolute path to the WAV file on success, or None on failure.
    The caller (C++ side) is responsible for deleting the file.
    The WAV is written to a persistent temp dir (NOT a TemporaryDirectory context)
    so the file survives this function's return.
    """
    binary = _find_ytdlp_binary()
    if not binary:
        return None

    # Create a persistent temp dir — NOT TemporaryDirectory(), which auto-deletes.
    audio_dir = tempfile.mkdtemp(prefix="ot_audio_")
    wav_path = os.path.join(audio_dir, "audio.wav")

    cmd = [
        binary,
        "--no-playlist",
        "--extract-audio",
        "--audio-format", "wav",
        "--postprocessor-args", "ffmpeg:-ar 16000 -ac 1",
        "--quiet",
        "--no-warnings",
        "-o", wav_path,
        url,
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if result.returncode == 0 and os.path.isfile(wav_path):
            return wav_path
    except Exception:
        pass
    return None


def fetch_transcript(url):
    """
    Fetch transcript for the given video URL.
    Returns a dict matching the expected JSON schema.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        title = ""
        source_type = "auto_captions"

        # Try the Python module first (faster, no subprocess overhead)
        module_available = False
        try:
            import yt_dlp  # noqa
            module_available = True
        except ImportError:
            module_available = False

        # manual_langs: set of language codes with genuine manual subtitles
        # (populated from the info dict — the only reliable signal)
        manual_langs = set()

        if module_available:
            try:
                title, manual_langs = _fetch_via_module(url, tmpdir)
            except Exception as exc:
                err_msg = str(exc)
                # Detect 403 / blocked
                if "403" in err_msg or "Forbidden" in err_msg or "HTTP Error 403" in err_msg:
                    return {
                        "success": False,
                        "error": "The publisher blocked automated access (HTTP 403)",
                        "error_code": "BLOCKED",
                    }
                # Fall through to binary on other errors
                module_available = False

        if not module_available:
            # Binary fallback
            binary = _find_ytdlp_binary()
            if not binary:
                return {
                    "success": False,
                    "error": "yt-dlp not found. Install it: brew install yt-dlp",
                    "error_code": "MISSING_DEPENDENCY",
                }
            returncode, stderr = _fetch_via_binary(url, tmpdir)
            if returncode != 0:
                if "403" in stderr or "Forbidden" in stderr or "HTTP Error 403" in stderr:
                    return {
                        "success": False,
                        "error": "The publisher blocked automated access (HTTP 403)",
                        "error_code": "BLOCKED",
                    }
                if returncode != 0 and not glob.glob(os.path.join(tmpdir, "*.vtt")):
                    return {
                        "success": False,
                        "error": f"yt-dlp failed (exit {returncode}): {stderr[:300]}",
                    }
            # Get title + manual_langs via info dict (single --dump-json call)
            title, manual_langs = _get_info_via_binary(url)

        # Find downloaded VTT files (glob — filename is not predictable)
        vtt_files = glob.glob(os.path.join(tmpdir, "*.vtt"))
        if not vtt_files:
            # No captions — attempt audio download for on-device speech recognition.
            audio_path = _download_audio(url)
            if audio_path:
                return {
                    "success": False,
                    "error": "No captions available for this video",
                    "error_code": "NO_CAPTIONS",
                    "audio_path": audio_path,
                    "title": title,
                }
            return {
                "success": False,
                "error": "No captions available for this video",
                "error_code": "NO_CAPTIONS",
            }

        # Classify each vtt file: extract the language code from the filename
        # (yt-dlp uses <id>.<lang>.vtt — the lang is everything between the last
        # two dots that is not "vtt").  A file is manual iff its lang code appears
        # in manual_langs (from info['subtitles']).
        def _lang_from_path(p):
            """Extract lang code from <id>.<lang>.vtt filename."""
            name = os.path.splitext(os.path.basename(p))[0]  # strip .vtt
            # name is now e.g. "jNQXAC9IVRw.en" or "jNQXAC9IVRw.en-en"
            parts = name.split(".")
            return parts[-1] if len(parts) >= 2 else ""

        manual_files = [f for f in vtt_files if _lang_from_path(f) in manual_langs]

        if manual_files:
            chosen = manual_files[0]
            source_type = "manual_captions"
        else:
            chosen = vtt_files[0]
            source_type = "auto_captions"

        with open(chosen, "r", encoding="utf-8", errors="replace") as f:
            vtt_text = f.read()

        transcript = _parse_vtt(vtt_text)

        if not transcript.strip():
            # VTT file was empty — fall back to audio download.
            audio_path = _download_audio(url)
            if audio_path:
                return {
                    "success": False,
                    "error": "No captions available for this video",
                    "error_code": "NO_CAPTIONS",
                    "audio_path": audio_path,
                    "title": title,
                }
            return {
                "success": False,
                "error": "No captions available for this video",
                "error_code": "NO_CAPTIONS",
            }

        word_count = len(transcript.split())

        return {
            "success": True,
            "title": title,
            "transcript": transcript,
            "source": source_type,
            "word_count": word_count,
        }


def main():
    if len(sys.argv) < 2:
        result = {
            "success": False,
            "error": "Usage: video_transcript.py <video_url>",
        }
        print(json.dumps(result))
        sys.exit(0)

    url = sys.argv[1]

    try:
        result = fetch_transcript(url)
    except Exception as exc:
        result = {
            "success": False,
            "error": str(exc),
        }

    # Only write to stdout; any debug/progress goes to stderr.
    print(json.dumps(result))
    sys.exit(0)


if __name__ == "__main__":
    main()
