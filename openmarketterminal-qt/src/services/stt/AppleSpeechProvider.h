#pragma once

#include <memory>

namespace openmarketterminal::services {
class SttProvider;
std::unique_ptr<SttProvider> create_apple_speech_provider();
}
