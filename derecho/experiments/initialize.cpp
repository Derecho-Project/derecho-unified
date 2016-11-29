
#include "rdmc/rdmc.h"
#include "sst/sst.h"

#include "initialize.h"

#include <map>

std::map<uint32_t, std::string> initialize (uint32_t &node_rank, uint32_t &num_nodes) {
	std::map<uint32_t, std::string> node_addresses;

	rdmc::query_addresses(node_addresses, node_rank);
    num_nodes = node_addresses.size();

    // initialize RDMA resources, input number of nodes, node rank and ip addresses and create TCP connections
    rdmc::initialize(node_addresses, node_rank);
    return node_addresses;
}