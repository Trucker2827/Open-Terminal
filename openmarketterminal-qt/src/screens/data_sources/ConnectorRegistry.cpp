// ConnectorRegistry.cpp — force-link all connector registration units
#include "screens/data_sources/ConnectorRegistry.h"

namespace openmarketterminal::screens::datasources {

void force_link_alternative_data_connectors();
void force_link_api_streaming_connectors();
void force_link_cloud_storage_connectors();
void force_link_file_source_connectors();
void force_link_market_data_connectors();
void force_link_nosql_database_connectors();
void force_link_open_banking_connectors();
void force_link_relational_database_connectors();
void force_link_search_warehouse_connectors();
void force_link_timeseries_database_connectors();

// Each connector .cpp file uses a static-init bool to self-register.
// This function exists solely to ensure the linker includes all those
// translation units — call it once from DataSourcesScreen constructor.
void register_all_connectors() {
    force_link_alternative_data_connectors();
    force_link_api_streaming_connectors();
    force_link_cloud_storage_connectors();
    force_link_file_source_connectors();
    force_link_market_data_connectors();
    force_link_nosql_database_connectors();
    force_link_open_banking_connectors();
    force_link_relational_database_connectors();
    force_link_search_warehouse_connectors();
    force_link_timeseries_database_connectors();
    (void)ConnectorRegistry::instance().count();
}

} // namespace openmarketterminal::screens::datasources
