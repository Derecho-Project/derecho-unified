/**
 * @file ViewManager.cpp
 *
 * @date Feb 6, 2017
 */

#include <arpa/inet.h>

#include "derecho_exception.h"
#include "view_manager.h"
#include "replicated.h" //Needed for the ReplicatedObject interface
#include <persistent/Persistent.hpp>

namespace derecho {

using lock_guard_t = std::lock_guard<std::mutex>;
using unique_lock_t = std::unique_lock<std::mutex>;
using shared_lock_t = std::shared_lock<std::shared_timed_mutex>;

/* Leader/Restart Leader Constructor */
ViewManager::ViewManager(const node_id_t my_id,
                         const ip_addr my_ip,
                         CallbackSet callbacks,
                         const SubgroupInfo& subgroup_info,
                         const DerechoParams& derecho_params,
                         ReplicatedObjectReferenceMap& object_reference_map,
                         const persistence_manager_callbacks_t& _persistence_manager_callbacks,
                         std::vector<view_upcall_t> _view_upcalls,
                         const int gms_port)
        : logger(spdlog::get("debug_log")),
          gms_port(gms_port),
          curr_view(persistent::loadObject<View>()), //Attempt to load a saved View from disk, to see if one is there
          server_socket(gms_port),
          thread_shutdown(false),
          view_upcalls(_view_upcalls),
          subgroup_info(subgroup_info),
          derecho_params(derecho_params),
          subgroup_objects(object_reference_map),
          persistence_manager_callbacks(_persistence_manager_callbacks) {
    std::map<subgroup_id_t, SubgroupSettings> subgroup_settings_map;
    uint32_t num_received_size = 0;
    //Determine if a saved view was loaded from disk
    if(curr_view != nullptr) {
        SPDLOG_DEBUG(logger, "Found view {} on disk, attempting to recover", curr_view->vid);
        load_ragged_trim();
        await_rejoining_nodes(my_id, subgroup_settings_map, num_received_size);

        curr_view->my_rank = curr_view->rank_of(my_id);

    } else {
        //Normal startup as the leader
        curr_view = std::make_unique<View>(0, std::vector<node_id_t>{my_id}, std::vector<ip_addr>{my_ip},
                                           std::vector<char>{0}, std::vector<node_id_t>{}, std::vector<node_id_t>{}, 0);
        await_first_view(my_id, subgroup_settings_map, num_received_size);
        curr_view->my_rank = curr_view->rank_of(my_id);
    }
    last_suspected = std::vector<bool>(curr_view->members.size());
    persistent::saveObject(*curr_view);
    initialize_rdmc_sst();
    SPDLOG_DEBUG(logger, "Initializing SST and RDMC for the first time.");
    construct_multicast_group(callbacks, derecho_params, subgroup_settings_map, num_received_size);

}

/* Non-leader Constructor */
ViewManager::ViewManager(const node_id_t my_id,
                         tcp::socket& leader_connection,
                         CallbackSet callbacks,
                         const SubgroupInfo& subgroup_info,
                         ReplicatedObjectReferenceMap& object_reference_map,
                         const persistence_manager_callbacks_t& _persistence_manager_callbacks,
                         std::vector<view_upcall_t> _view_upcalls,
                         const int gms_port)
        : logger(spdlog::get("debug_log")),
          gms_port(gms_port),
          curr_view(persistent::loadObject<View>()),
          server_socket(gms_port),
          thread_shutdown(false),
          view_upcalls(_view_upcalls),
          subgroup_info(subgroup_info),
          derecho_params(0, 0),
          subgroup_objects(object_reference_map),
          persistence_manager_callbacks(_persistence_manager_callbacks) {
    //First, receive the view and parameters over the given socket
    bool is_total_restart = receive_configuration(my_id, leader_connection);

    //Set this while we still know my_id
    curr_view->my_rank = curr_view->rank_of(my_id);
    persistent::saveObject(*curr_view);
    last_suspected = std::vector<bool>(curr_view->members.size());
    initialize_rdmc_sst();
    std::map<subgroup_id_t, SubgroupSettings> subgroup_settings_map;
    uint32_t num_received_size;
    if(is_total_restart) {
        //Todo next: write derive_subgroup_settings assuming curr_view already has subgroup_shard_views but my_subgroups is wrong
    } else {
        num_received_size = make_subgroup_maps(std::unique_ptr<View>(), *curr_view, subgroup_settings_map);
    };
    SPDLOG_DEBUG(logger, "Initializing SST and RDMC for the first time.");
    construct_multicast_group(callbacks, derecho_params, subgroup_settings_map, num_received_size);

    curr_view->gmsSST->vid[curr_view->my_rank] = curr_view->vid;

}

ViewManager::~ViewManager() {
    thread_shutdown = true;
    // force accept to return.
    tcp::socket s{"localhost", gms_port};
    if(client_listener_thread.joinable()) {
        client_listener_thread.join();
    }
    old_views_cv.notify_all();
    if(old_view_cleanup_thread.joinable()) {
        old_view_cleanup_thread.join();
    }
}

/* ----------  1. Constructor Components ------------- */

bool ViewManager::receive_configuration(node_id_t my_id, tcp::socket& leader_connection) {
    JoinResponse leader_response;
    bool leader_redirect;
    do {
        leader_redirect = false;
        SPDLOG_DEBUG(logger, "Socket connected to leader, exchanging IDs.");
        leader_connection.write(my_id);
        leader_connection.read(leader_response);
        if(leader_response.code == JoinResponseCode::ID_IN_USE) {
            logger->error("Error! Leader refused connection because ID {} is already in use!", my_id);
            logger->flush();
            throw derecho_exception("Leader rejected join, ID already in use");
        }
        if(leader_response.code == JoinResponseCode::LEADER_REDIRECT) {
            std::size_t ip_addr_size;
            leader_connection.read(ip_addr_size);
            char buffer[ip_addr_size];
            leader_connection.read(buffer, ip_addr_size);
            ip_addr leader_ip(buffer);
            SPDLOG_DEBUG(logger, "That node was not the leader! Redirecting to {}", leader_ip);
            //Use move-assignment to reconnect the socket to the given IP address, and try again
            //(good thing that leader_connection reference is mutable)
            leader_connection = tcp::socket(leader_ip, gms_port);
            leader_redirect = true;
        }
    } while(leader_redirect);
    node_id_t leader_id = leader_response.leader_id;
    bool is_total_restart = (leader_response.code == JoinResponseCode::TOTAL_RESTART);
    if(is_total_restart) {
        SPDLOG_DEBUG(logger, "In restart mode, sending view {} to leader", curr_view->vid);
        leader_connection.write(mutils::bytes_size(*curr_view));
        auto leader_socket_write = [&leader_connection](const char* bytes, std::size_t size) {
            leader_connection.write(bytes, size);
        };
        mutils::post_object(leader_socket_write, *curr_view);
        load_ragged_trim();
        //Protocol: Send the number of RaggedTrim objects, then serialize each RaggedTrim
        leader_connection.write(logged_ragged_trim.size());
        for(const auto& ragged_trim_pair : logged_ragged_trim) {
            leader_connection.write(mutils::bytes_size(*ragged_trim_pair.second));
            mutils::post_object(leader_socket_write, *ragged_trim_pair.second);
        }
    }
    /* This second ID exchange is really a "heartbeat" to assure the leader
     * the client is still alive by the time it's ready to send the view */
    leader_connection.exchange(my_id, leader_id);
    //The leader will first send the size of the necessary buffer, then the serialized View
    std::size_t size_of_view;
    bool success = leader_connection.read(size_of_view);
    assert(success);
    char buffer[size_of_view];
    success = leader_connection.read(buffer, size_of_view);
    assert(success);
    if(is_total_restart) {
        //In total restart mode the leader sends a complete View, including all SubViews
        curr_view = mutils::from_bytes<View>(nullptr, buffer);
    } else {
        //This alternate from_bytes is needed because the leader didn't serialize the SubViews
        curr_view = StreamlinedView::view_from_bytes(nullptr, buffer);
    }
    //Next, the leader sends DerechoParams
    std::size_t size_of_derecho_params;
    success = leader_connection.read(size_of_derecho_params);
    char buffer2[size_of_derecho_params];
    success = leader_connection.read(buffer2, size_of_derecho_params);
    assert(success);
    derecho_params = *mutils::from_bytes<DerechoParams>(nullptr, buffer2);
    if(is_total_restart) {
        SPDLOG_DEBUG(logger, "In restart mode, receiving ragged trim from leader");
        logged_ragged_trim.clear();
        std::size_t num_of_ragged_trims;
        leader_connection.read(num_of_ragged_trims);
        for(std::size_t i = 0; i < num_of_ragged_trims; ++i) {
            std::size_t size_of_ragged_trim;
            leader_connection.read(size_of_ragged_trim);
            char buffer[size_of_ragged_trim];
            leader_connection.read(buffer, size_of_ragged_trim);
            std::unique_ptr<RaggedTrim> ragged_trim = mutils::from_bytes<RaggedTrim>(nullptr, buffer);
            logged_ragged_trim.emplace(ragged_trim->subgroup_id, std::move(ragged_trim));
        }
    }
    return is_total_restart;
}

void ViewManager::finish_setup(const std::shared_ptr<tcp::tcp_connections>& group_tcp_sockets) {
    group_member_sockets = group_tcp_sockets;
    curr_view->gmsSST->put();
    curr_view->gmsSST->sync_with_members();
    SPDLOG_DEBUG(logger, "Done setting up initial SST and RDMC");

    if(curr_view->vid != 0) {
        // If this node is joining an existing group with a non-initial view, copy the leader's num_changes, num_acked, and num_committed
        // Otherwise, you'll immediately think that there's a new proposed view change because gmsSST.num_changes[leader] > num_acked[my_rank]
        curr_view->gmsSST->init_local_change_proposals(curr_view->rank_of_leader());
        curr_view->gmsSST->put();
        SPDLOG_DEBUG(logger, "Joining node initialized its SST row from the leader");
    }

    create_threads();
    register_predicates();

    shared_lock_t lock(view_mutex);
    for(auto& view_upcall : view_upcalls) {
        view_upcall(*curr_view);
    }
}

void ViewManager::start(const std::unique_ptr<std::vector<std::vector<int64_t>>>& old_shard_leaders_for_restart) {
    //If this node is doing a total restart, it should now truncate its logs based on the last ragged trim
    //(This can't be done earlier, because this is the first point at which Replicated<T> objects are constructed)
    if(!logged_ragged_trim.empty()) {
        truncate_persistent_logs(logged_ragged_trim);
        if(old_shard_leaders_for_restart) {
            old_shard_leaders = *old_shard_leaders_for_restart;
        }
        //Now, send the logs to any restarting nodes who are new members of the view
        send_objects_to_new_members(old_shard_leaders);
        //Once this is finished, we no longer need logged_ragged_trim or old_shard_leaders
        logged_ragged_trim.clear();
        old_shard_leaders.clear();
    }
    SPDLOG_DEBUG(logger, "Starting predicate evaluation");
    curr_view->gmsSST->start_predicate_evaluation();
}

void ViewManager::load_ragged_trim() {
    for(const auto& subgroup_shard_pair : curr_view->my_subgroups) {
        std::string ragged_trim_filename = "RaggedTrim_" + std::to_string(subgroup_shard_pair.first);
        auto ragged_trim = persistent::loadObject<RaggedTrim>(ragged_trim_filename.c_str());
        if(ragged_trim == nullptr) {
            logger->error("No ragged trim information found for Subgroup {}!", subgroup_shard_pair.first);
            logger->flush();
            //what do I do in this case? Try to download ragged trim information from someone else?
            throw derecho_exception("Error! Can't recover with partial logs.");
        }
        logged_ragged_trim.emplace(subgroup_shard_pair.first, std::move(ragged_trim));
    }
}


void ViewManager::truncate_persistent_logs(const std::map<subgroup_id_t, std::unique_ptr<RaggedTrim>>& logged_ragged_trims) {
    for(const auto& id_trim_pair : logged_ragged_trims) {
        subgroup_id_t subgroup_id = id_trim_pair.first;
        const auto& ragged_trim = id_trim_pair.second;
        uint32_t num_shard_senders = ragged_trim->max_received_by_sender.size();
        //Determine the last deliverable sequence number using the same logic as deliver_messages_upto
        int32_t max_seq_num = 0;
        for(uint sender = 0; sender < num_shard_senders; sender++) {
            max_seq_num = std::max(max_seq_num,
                               static_cast<int32_t>(ragged_trim->max_received_by_sender[sender] * num_shard_senders + sender));
        }
        //Make the corresponding version number using the same logic as version_message
        persistent::version_t max_delivered_version = persistent::combine_int32s(ragged_trim->vid, max_seq_num);
        //Call Persistent::truncate() with this version
        subgroup_objects.at(subgroup_id).get().truncate(max_delivered_version);
    }
}

void ViewManager::await_first_view(const node_id_t my_id,
                                   std::map<subgroup_id_t, SubgroupSettings>& subgroup_settings,
                                   uint32_t& num_received_size) {
    std::list<tcp::socket> waiting_join_sockets;
    curr_view->is_adequately_provisioned = false;
    bool joiner_failed = false;
    std::list<tcp::socket>::iterator last_checked_joiner;
    do {
        while(!curr_view->is_adequately_provisioned) {
            tcp::socket client_socket = server_socket.accept();
            node_id_t joiner_id = 0;
            client_socket.read(joiner_id);
            if(curr_view->rank_of(joiner_id) != -1) {
                client_socket.write(JoinResponse{JoinResponseCode::ID_IN_USE, my_id});
                continue;
            }
            client_socket.write(JoinResponse{JoinResponseCode::OK, my_id});
            const ip_addr& joiner_ip = client_socket.get_remote_ip();
            ip_addr my_ip = client_socket.get_self_ip();
            //Construct a new view by appending this joiner to the previous view
            //None of these views are ever installed, so we don't use curr_view/next_view like normal
            curr_view = std::make_unique<View>(curr_view->vid,
                                               functional_append(curr_view->members, joiner_id),
                                               functional_append(curr_view->member_ips, joiner_ip),
                                               std::vector<char>(curr_view->num_members + 1, 0),
                                               functional_append(curr_view->joined, joiner_id));
            num_received_size = make_subgroup_maps(std::unique_ptr<View>(), *curr_view, subgroup_settings);
            waiting_join_sockets.emplace_back(std::move(client_socket));
        }
        /* Now that enough joiners are queued up to make an adequate view,
         * test to see if any of them have failed while waiting to join by
         * exchanging some trivial data and checking the TCP socket result */
        if(!joiner_failed) {
            //initialize this pointer on the first iteration of the outer loop
            last_checked_joiner = waiting_join_sockets.begin();
        }
        joiner_failed = false;
        //Starting at the position we left off ensures the earlier non-failed nodes won't get multiple exchanges
        for(auto waiting_sockets_iter = last_checked_joiner;
                waiting_sockets_iter != waiting_join_sockets.end(); ) {
            ip_addr joiner_ip = waiting_sockets_iter->get_remote_ip();
            node_id_t joiner_id = 0;
            bool write_success = waiting_sockets_iter->exchange(my_id, joiner_id);
            if(!write_success) {
                //Remove the failed client and try again
                waiting_sockets_iter = waiting_join_sockets.erase(waiting_sockets_iter);
                std::vector<node_id_t> filtered_members(curr_view->members.size() - 1);
                std::vector<ip_addr> filtered_ips(curr_view->member_ips.size() - 1);
                std::vector<node_id_t> filtered_joiners(curr_view->joined.size() - 1);
                std::remove_copy(curr_view->members.begin(), curr_view->members.end(),
                                 filtered_members.begin(), joiner_id);
                std::remove_copy(curr_view->member_ips.begin(), curr_view->member_ips.end(),
                                 filtered_ips.begin(), joiner_ip);
                std::remove_copy(curr_view->joined.begin(), curr_view->joined.end(),
                                 filtered_joiners.begin(), joiner_id);
                curr_view = std::make_unique<View>(0, filtered_members, filtered_ips,
                                                   std::vector<char>(curr_view->num_members - 1, 0), filtered_joiners);
                /* This will update curr_view->is_adequately_provisioned, so now we must
                 * start over from the beginning and test if we need to wait for more joiners. */
                num_received_size = make_subgroup_maps(std::unique_ptr<View>(), *curr_view, subgroup_settings);
                joiner_failed = true;
                break;
            }
            last_checked_joiner = waiting_sockets_iter;
            ++waiting_sockets_iter;
        }
        if(joiner_failed) continue;
        // If none of the joining nodes have failed, we can continue sending them all the view
        while(!waiting_join_sockets.empty()) {
            auto bind_socket_write = [&waiting_join_sockets](const char* bytes, std::size_t size) {
                bool success = waiting_join_sockets.front().write(bytes, size);
                assert(success);
            };
            mutils::post_object(bind_socket_write, mutils::bytes_size(*curr_view));
            mutils::post_object(bind_socket_write, *curr_view);
            waiting_join_sockets.front().write(mutils::bytes_size(derecho_params));
            mutils::post_object(bind_socket_write, derecho_params);
            //Send a "0" as the size of the "old shard leaders" vector, since there are no old leaders
            mutils::post_object(bind_socket_write, std::size_t{0});
            waiting_join_sockets.pop_front();
        }
    } while(joiner_failed);
}

void ViewManager::await_rejoining_nodes(const node_id_t my_id,
                                        std::map<subgroup_id_t, SubgroupSettings>& subgroup_settings,
                                        uint32_t& num_received_size) {
    std::map<node_id_t, tcp::socket> waiting_join_sockets;
    std::set<node_id_t> rejoined_node_ids;
    std::set<node_id_t> last_known_view_members(curr_view->members.begin(), curr_view->members.end());
    std::unique_ptr<View> restart_view;
    node_id_t last_checked_joiner = 0;
    rejoined_node_ids.emplace(my_id);
    //Wait for a majority of nodes from the last known view to join
    bool ready_to_restart = false;
    int time_remaining_ms = RESTART_LEADER_TIMEOUT;
    while(time_remaining_ms > 0) {
        auto start_time = std::chrono::high_resolution_clock::now();
        std::experimental::optional<tcp::socket> client_socket = server_socket.try_accept(time_remaining_ms);
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::milliseconds time_waited = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        time_remaining_ms -= time_waited.count();
        if(client_socket) {
            node_id_t joiner_id = 0;
            client_socket->read(joiner_id);
            client_socket->write(JoinResponse{JoinResponseCode::TOTAL_RESTART, my_id});
            SPDLOG_DEBUG(logger, "Node {} rejoined", joiner_id);
            rejoined_node_ids.emplace(joiner_id);

            //Receive the joining node's saved View
            std::size_t size_of_view;
            client_socket->read(size_of_view);
            char view_buffer[size_of_view];
            client_socket->read(view_buffer, size_of_view);
            auto client_view = mutils::from_bytes<View>(nullptr, view_buffer);

            if(client_view->vid <= curr_view->vid) {
                //Read the ragged trims from the client, but throw them away, because the current view is newer
                std::size_t num_of_ragged_trims;
                client_socket->read(num_of_ragged_trims);
                for(std::size_t i = 0; i < num_of_ragged_trims; ++i) {
                    std::size_t size_of_ragged_trim;
                    client_socket->read(size_of_ragged_trim);
                    char buffer[size_of_ragged_trim];
                    client_socket->read(buffer, size_of_ragged_trim);
                }
            } else {
                //The joining node has a newer View, so update our View and receive the ragged trims
                curr_view.swap(client_view);
                logged_ragged_trim.clear();
                std::size_t num_of_ragged_trims;
                client_socket->read(num_of_ragged_trims);
                for(std::size_t i = 0; i < num_of_ragged_trims; ++i) {
                    std::size_t size_of_ragged_trim;
                    client_socket->read(size_of_ragged_trim);
                    char buffer[size_of_ragged_trim];
                    client_socket->read(buffer, size_of_ragged_trim);
                    std::unique_ptr<RaggedTrim> ragged_trim = mutils::from_bytes<RaggedTrim>(nullptr, buffer);
                    logged_ragged_trim.emplace(ragged_trim->subgroup_id, std::move(ragged_trim));
                }
                //Remake the std::set version of curr_view->members
                last_known_view_members.clear();
                last_known_view_members.insert(curr_view->members.begin(), curr_view->members.end());
            }
            waiting_join_sockets.emplace(joiner_id, std::move(*client_socket));
            //Compute the intersection of rejoined_node_ids and last_known_view_members
            //in the most clumsy, verbose, awkward way possible
            std::set<node_id_t> intersection_of_ids;
            std::set_intersection(rejoined_node_ids.begin(), rejoined_node_ids.end(), 
                                  last_known_view_members.begin(), last_known_view_members.end(), 
                                  std::inserter(intersection_of_ids, intersection_of_ids.end()));
            if(intersection_of_ids.size() >= (last_known_view_members.size() / 2) + 1) {
                //A majority of the last known view has reconnected, now determine if the new view would be adequate
                restart_view = update_curr_and_next_restart_view(waiting_join_sockets, rejoined_node_ids);
                num_received_size = make_subgroup_maps(curr_view, *restart_view, subgroup_settings);
                if(restart_view->is_adequately_provisioned) {
                    //Keep waiting for more than a quorum if the next view would not be adequate
                    ready_to_restart = true;
                }
            }
            //If we're about to restart, go back and test to see if any joining nodes have since failed
            if(ready_to_restart) {
                if(last_checked_joiner == 0) {
                    last_checked_joiner = waiting_join_sockets.begin()->first;
                }
                //Don't re-exchange with joiners already tested; this works because std::map is sorted
                for(auto waiting_sockets_iter = waiting_join_sockets.find(last_checked_joiner);
                        waiting_sockets_iter != waiting_join_sockets.end(); ) {
                     node_id_t joiner_id = 0;
                     bool write_success = waiting_sockets_iter->second.exchange(my_id, joiner_id);
                     if(!write_success) {
                         waiting_sockets_iter = waiting_join_sockets.erase(waiting_sockets_iter);
                         rejoined_node_ids.erase(waiting_sockets_iter->first);
                         ready_to_restart = false;
                         break;
                     }
                     last_checked_joiner = waiting_sockets_iter->first;
                     ++waiting_sockets_iter;
                }
            }
            //If all the members have rejoined, no need to keep waiting
            if(intersection_of_ids.size() == last_known_view_members.size() && ready_to_restart) {
                break;
            }
        } else if(!ready_to_restart) {
            //Accept timed out, but we haven't heard from enough nodes yet, so reset the timer
            time_remaining_ms = RESTART_LEADER_TIMEOUT;
        }
    }
    assert(restart_view);
    SPDLOG_DEBUG(logger, "Reached a quorum of nodes from view {}, installing view {}", curr_view->vid, restart_view->vid);

    std::vector<std::vector<int64_t>> old_shard_leaders_by_id =
            translate_types_to_ids(make_shard_leaders_map(*curr_view), *restart_view);
    curr_view.swap(restart_view);
    //Send the next view to all the members
    for(auto waiting_sockets_iter = waiting_join_sockets.begin(); 
            waiting_sockets_iter != waiting_join_sockets.end(); ) {
        auto bind_socket_write = [&waiting_sockets_iter](const char* bytes, std::size_t size) {
            bool success = waiting_sockets_iter->second.write(bytes, size);
            assert(success);
        };
        SPDLOG_DEBUG(logger, "Sending post-recovery view {} to node {}", curr_view->vid, waiting_sockets_iter->first);
        mutils::post_object(bind_socket_write, mutils::bytes_size(*curr_view));
        mutils::post_object(bind_socket_write, *curr_view);
        mutils::post_object(bind_socket_write, mutils::bytes_size(derecho_params));
        mutils::post_object(bind_socket_write, derecho_params);
        SPDLOG_DEBUG(logger, "Sending ragged-trim information to node {}", waiting_sockets_iter->first);
        waiting_sockets_iter->second.write(logged_ragged_trim.size());
        for(const auto& ragged_trim_pair : logged_ragged_trim) {
            mutils::post_object(bind_socket_write, mutils::bytes_size(*ragged_trim_pair.second));
            mutils::post_object(bind_socket_write, *ragged_trim_pair.second);
        }
        //Next, the joining node will expect a vector of shard leaders from which to receive Persistent logs
        mutils::post_object(bind_socket_write, mutils::bytes_size(old_shard_leaders_by_id));
        mutils::post_object(bind_socket_write, old_shard_leaders_by_id);
        //Put the sockets here, where subsequent methods will send them Persistent logs for Replicated Objects
        proposed_join_sockets.emplace_back(std::move(waiting_sockets_iter->second));
        waiting_sockets_iter = waiting_join_sockets.erase(waiting_sockets_iter);
    }
    //Save this to a class member so that we still have it in start()...ugh, that's ugly
    old_shard_leaders = old_shard_leaders_by_id;
}

std::unique_ptr<View> ViewManager::update_curr_and_next_restart_view(const std::map<node_id_t, tcp::socket>& waiting_join_sockets,
                                                                     const std::set<node_id_t>& rejoined_node_ids) {
    //Nodes that were not in the last view but have restarted will immediately "join" in the new view
    std::vector<node_id_t> nodes_to_add_in_next_view;
    std::vector<ip_addr> ips_to_add_in_next_view;
    for(const auto& id_socket_pair : waiting_join_sockets) {
        node_id_t joiner_id = id_socket_pair.first;
        int joiner_rank = curr_view->rank_of(joiner_id);
        if(joiner_rank == -1) {
            nodes_to_add_in_next_view.emplace_back(joiner_id);
            ips_to_add_in_next_view.emplace_back(id_socket_pair.second.get_remote_ip());
            //If this node had been marked as failed, but was still in the view, un-fail it
        } else if(curr_view->failed[joiner_rank] == true) {
            curr_view->failed[joiner_rank] = false;
            curr_view->num_failed--;
        }
    }
    //Mark any nodes from the last view that didn't respond before the timeout as failed
    for(std::size_t rank = 0; rank < curr_view->members.size(); ++rank) {
        if(rejoined_node_ids.count(curr_view->members[rank]) == 0
                && !curr_view->failed[rank]) {
            curr_view->failed[rank] = true;
            curr_view->num_failed++;
        }
    }

    //Compute the next view, which will include all the members currently rejoining and remove the failed ones
    return make_next_view(curr_view, nodes_to_add_in_next_view, ips_to_add_in_next_view, logger);
}

void ViewManager::initialize_rdmc_sst() {
    // construct member_ips
    auto member_ips_map = make_member_ips_map(*curr_view);
    if(!rdmc::initialize(member_ips_map, curr_view->members[curr_view->my_rank])) {
        std::cout << "Global setup failed" << std::endl;
        exit(0);
    }
    sst::verbs_initialize(member_ips_map, curr_view->members[curr_view->my_rank]);
}

std::map<node_id_t, ip_addr> ViewManager::make_member_ips_map(const View& view) {
    std::map<node_id_t, ip_addr> member_ips_map;
    size_t num_members = view.members.size();
    for(uint i = 0; i < num_members; ++i) {
        if(!view.failed[i]) {
            member_ips_map[view.members[i]] = view.member_ips[i];
        }
    }
    return member_ips_map;
}

void ViewManager::create_threads() {
    client_listener_thread = std::thread{[this]() {
        pthread_setname_np(pthread_self(), "client_thread");
        while(!thread_shutdown) {
            tcp::socket client_socket = server_socket.accept();
            SPDLOG_DEBUG(logger, "Background thread got a client connection from {}", client_socket.get_remote_ip());
            pending_join_sockets.locked().access.emplace_back(std::move(client_socket));
        }
        std::cout << "Connection listener thread shutting down." << std::endl;
    }};

    old_view_cleanup_thread = std::thread([this]() {
        pthread_setname_np(pthread_self(), "old_view");
        while(!thread_shutdown) {
            unique_lock_t old_views_lock(old_views_mutex);
            old_views_cv.wait(old_views_lock, [this]() {
                return !old_views.empty() || thread_shutdown;
            });
            if(!thread_shutdown) {
                old_views.front().reset();
                old_views.pop();
            }
        }
        std::cout << "Old View cleanup thread shutting down." << std::endl;
    });
}

void ViewManager::register_predicates() {
    /* Note that each trigger function must be wrapped in a lambda because it's
     * a member function, and lambdas are the only way to bind "this" to a member
     * function invocation. */
    auto suspected_changed = [this](const DerechoSST& sst) {
        return suspected_not_equal(sst, last_suspected);
    };
    auto suspected_changed_trig = [this](DerechoSST& sst) { new_suspicion(sst); };

    auto start_join_pred = [this](const DerechoSST& sst) {
        return curr_view->i_am_leader() && has_pending_join();
    };
    auto start_join_trig = [this](DerechoSST& sst) { leader_start_join(sst); };

    auto reject_join_pred = [this](const DerechoSST& sst) {
        return !curr_view->i_am_leader() && has_pending_join();
    };
    auto reject_join = [this](DerechoSST& sst) { redirect_join_attempt(sst); };

    auto change_commit_ready = [this](const DerechoSST& gmsSST) {
        return curr_view->i_am_leader()
               && min_acked(gmsSST, curr_view->failed) > gmsSST.num_committed[gmsSST.get_local_index()];
    };
    auto commit_change = [this](DerechoSST& sst) { leader_commit_change(sst); };

    auto leader_proposed_change = [this](const DerechoSST& gmsSST) {
        return gmsSST.num_changes[curr_view->rank_of_leader()]
               > gmsSST.num_acked[gmsSST.get_local_index()];
    };
    auto ack_proposed_change = [this](DerechoSST& sst) { acknowledge_proposed_change(sst); };

    auto leader_committed_changes = [this](const DerechoSST& gmsSST) {
        return gmsSST.num_committed[curr_view->rank_of_leader()]
               > gmsSST.num_installed[curr_view->my_rank];
    };
    auto view_change_trig = [this](DerechoSST& sst) { start_meta_wedge(sst); };

    if(!suspected_changed_handle.is_valid()) {
        suspected_changed_handle = curr_view->gmsSST->predicates.insert(suspected_changed, suspected_changed_trig,
                                                                        sst::PredicateType::RECURRENT);
    }
    if(!start_join_handle.is_valid()) {
        start_join_handle = curr_view->gmsSST->predicates.insert(start_join_pred, start_join_trig,
                                                                 sst::PredicateType::RECURRENT);
    }
    if(!reject_join_handle.is_valid()) {
        reject_join_handle = curr_view->gmsSST->predicates.insert(reject_join_pred, reject_join,
                                                                  sst::PredicateType::RECURRENT);
    }
    if(!change_commit_ready_handle.is_valid()) {
        change_commit_ready_handle = curr_view->gmsSST->predicates.insert(change_commit_ready, commit_change,
                                                                          sst::PredicateType::RECURRENT);
    }
    if(!leader_proposed_handle.is_valid()) {
        leader_proposed_handle = curr_view->gmsSST->predicates.insert(leader_proposed_change, ack_proposed_change,
                                                                      sst::PredicateType::RECURRENT);
    }
    if(!leader_committed_handle.is_valid()) {
        leader_committed_handle = curr_view->gmsSST->predicates.insert(leader_committed_changes, view_change_trig,
                                                                       sst::PredicateType::ONE_TIME);
    }
}

/* ------------- 2. Predicate-Triggers That Implement View Management Logic ---------- */

void ViewManager::new_suspicion(DerechoSST& gmsSST) {
    SPDLOG_DEBUG(logger, "Suspected[] changed");
    View& Vc = *curr_view;
    int myRank = curr_view->my_rank;
    // Aggregate suspicions into gmsSST[myRank].Suspected;
    for(int r = 0; r < Vc.num_members; r++) {
        for(int who = 0; who < Vc.num_members; who++) {
            gmssst::set(gmsSST.suspected[myRank][who], gmsSST.suspected[myRank][who] || gmsSST.suspected[r][who]);
        }
    }

    for(int q = 0; q < Vc.num_members; q++) {
        //If this is a new suspicion
        if(gmsSST.suspected[myRank][q] && !Vc.failed[q]) {
            SPDLOG_DEBUG(logger, "New suspicion: node {}", Vc.members[q]);
            //This is safer than copy_suspected, since suspected[] might change during this loop
            last_suspected[q] = gmsSST.suspected[myRank][q];
            if(Vc.num_failed >= (Vc.num_members + 1) / 2) {
                logger->flush();
                throw derecho_exception("Majority of a Derecho group simultaneously failed ... shutting down");
            }

            SPDLOG_DEBUG(logger, "GMS telling SST to freeze row {}", q);
            gmsSST.freeze(q);  // Cease to accept new updates from q
            Vc.multicast_group->wedge();
            gmssst::set(gmsSST.wedged[myRank], true);  // RDMC has halted new sends and receives in theView
            Vc.failed[q] = true;
            Vc.num_failed++;

            if(Vc.num_failed >= (Vc.num_members + 1) / 2) {
                logger->flush();
                throw derecho_exception("Potential partitioning event: this node is no longer in the majority and must shut down!");
            }

            // push change to gmsSST.suspected[myRank]
            gmsSST.put(gmsSST.suspected.get_base() - gmsSST.getBaseAddress(), gmsSST.changes.get_base() - gmsSST.suspected.get_base());
            // push change to gmsSST.wedged[myRank]
            gmsSST.put(gmsSST.wedged.get_base() - gmsSST.getBaseAddress(), sizeof(gmsSST.wedged[0]));
            if(Vc.i_am_leader() && !changes_contains(gmsSST, Vc.members[q]))  // Leader initiated
            {
                const int next_change_index = gmsSST.num_changes[myRank] - gmsSST.num_installed[myRank];
                if(next_change_index == (int)gmsSST.changes.size()) {
                    throw derecho_exception("Ran out of room in the pending changes list");
                }

                gmssst::set(gmsSST.changes[myRank][next_change_index], Vc.members[q]);  // Reports the failure (note that q NotIn members)
                gmssst::increment(gmsSST.num_changes[myRank]);
                SPDLOG_DEBUG(logger, "Leader proposed a change to remove failed node {}", Vc.members[q]);
                gmsSST.put((char*)std::addressof(gmsSST.changes[0][next_change_index]) - gmsSST.getBaseAddress(),
                           sizeof(gmsSST.changes[0][next_change_index]));
                gmsSST.put(gmsSST.num_changes.get_base() - gmsSST.getBaseAddress(), sizeof(gmsSST.num_changes[0]));
            }
        }
    }
}

void ViewManager::leader_start_join(DerechoSST& gmsSST) {
    SPDLOG_DEBUG(logger, "GMS handling a new client connection");
    {
        //Hold the lock on pending_join_sockets while moving a socket into proposed_join_sockets
        auto pending_join_sockets_locked = pending_join_sockets.locked();
        proposed_join_sockets.splice(proposed_join_sockets.end(), pending_join_sockets_locked.access, pending_join_sockets_locked.access.begin());
    }
    bool success = receive_join(proposed_join_sockets.back());
    //If the join failed, close the socket
    if(!success) proposed_join_sockets.pop_back();
}

void ViewManager::redirect_join_attempt(DerechoSST& gmsSST) {
    tcp::socket client_socket;
    {
        auto pending_join_sockets_locked = pending_join_sockets.locked();
        client_socket = std::move(pending_join_sockets_locked.access.front());
        pending_join_sockets_locked.access.pop_front();
    }
    node_id_t joiner_id;
    client_socket.read(joiner_id);
    client_socket.write(JoinResponse{JoinResponseCode::LEADER_REDIRECT, curr_view->members[curr_view->my_rank]});
    //Send the client the IP address of the current leader
    client_socket.write(mutils::bytes_size(curr_view->member_ips[curr_view->rank_of_leader()]));
    auto bind_socket_write = [&client_socket](const char* bytes, std::size_t size) { client_socket.write(bytes, size); };
    mutils::post_object(bind_socket_write, curr_view->member_ips[curr_view->rank_of_leader()]);
}

void ViewManager::leader_commit_change(DerechoSST& gmsSST) {
    gmssst::set(gmsSST.num_committed[gmsSST.get_local_index()],
                min_acked(gmsSST, curr_view->failed));  // Leader commits a new request
    SPDLOG_DEBUG(logger, "Leader committing change proposal #{}", gmsSST.num_committed[gmsSST.get_local_index()]);
    gmsSST.put(gmsSST.num_committed.get_base() - gmsSST.getBaseAddress(), sizeof(gmsSST.num_committed[0]));
}

void ViewManager::acknowledge_proposed_change(DerechoSST& gmsSST) {
    int myRank = gmsSST.get_local_index();
    int leader = curr_view->rank_of_leader();
    SPDLOG_DEBUG(logger, "Detected that leader proposed change #{}. Acknowledging.", gmsSST.num_changes[leader]);
    if(myRank != leader) {
        // Echo the count
        gmssst::set(gmsSST.num_changes[myRank], gmsSST.num_changes[leader]);

        // Echo (copy) the vector including the new changes
        gmssst::set(gmsSST.changes[myRank], gmsSST.changes[leader], gmsSST.changes.size());
        // Echo the new member's IP
        gmssst::set(gmsSST.joiner_ips[myRank], gmsSST.joiner_ips[leader], gmsSST.joiner_ips.size());
        gmssst::set(gmsSST.num_committed[myRank], gmsSST.num_committed[leader]);
    }

    // Notice a new request, acknowledge it
    gmssst::set(gmsSST.num_acked[myRank], gmsSST.num_changes[myRank]);
    gmsSST.put(gmsSST.changes.get_base() - gmsSST.getBaseAddress(),
               gmsSST.num_received.get_base() - gmsSST.changes.get_base());
    SPDLOG_DEBUG(logger, "Wedging current view.");
    curr_view->wedge();
    SPDLOG_DEBUG(logger, "Done wedging current view.");
}

void ViewManager::start_meta_wedge(DerechoSST& gmsSST) {
    SPDLOG_DEBUG(logger, "Meta-wedging view {}", curr_view->vid);
    // Disable all the other SST predicates, except suspected_changed and the one I'm about to register
    gmsSST.predicates.remove(start_join_handle);
    gmsSST.predicates.remove(reject_join_handle);
    gmsSST.predicates.remove(change_commit_ready_handle);
    gmsSST.predicates.remove(leader_proposed_handle);

    curr_view->wedge();

    /* We now need to wait for all other nodes to wedge the current view,
     * which is called "meta-wedged." To do that, this predicate trigger
     * creates a new predicate that will fire when meta-wedged is true, and
     * registers the next epoch termination method as its trigger.
     */
    auto is_meta_wedged = [this](const DerechoSST& gmsSST) {
        for(unsigned int n = 0; n < gmsSST.get_num_rows(); ++n) {
            if(!curr_view->failed[n] && !gmsSST.wedged[n]) {
                return false;
            }
        }
        return true;
    };
    auto meta_wedged_continuation = [this](DerechoSST& gmsSST) {
        //Before the first call to terminate_epoch(), heap-allocate this map
        auto next_subgroup_settings = std::make_shared<std::map<subgroup_id_t, SubgroupSettings>>();
        terminate_epoch(next_subgroup_settings, 0, gmsSST);
    };
    gmsSST.predicates.insert(is_meta_wedged, meta_wedged_continuation, sst::PredicateType::ONE_TIME);
}

void ViewManager::terminate_epoch(std::shared_ptr<std::map<subgroup_id_t, SubgroupSettings>> next_subgroup_settings,
                                  uint32_t next_num_received_size,
                                  DerechoSST& gmsSST) {
    SPDLOG_DEBUG(logger, "MetaWedged is true; continuing epoch termination");
    //If this is the first time terminate_epoch() was called, next_view will still be null
    bool first_call = false;
    if(!next_view) {
        first_call = true;
    }
    std::unique_lock<std::shared_timed_mutex> write_lock(view_mutex);
    next_view = make_next_view(curr_view, gmsSST, logger);
    SPDLOG_DEBUG(logger, "Checking provisioning of view {}", next_view->vid);
    next_subgroup_settings->clear();
    next_num_received_size = make_subgroup_maps(curr_view, *next_view, *next_subgroup_settings);
    if(!next_view->is_adequately_provisioned) {
        SPDLOG_DEBUG(logger, "Next view would not be adequately provisioned, waiting for more joins.");
        if(first_call) {
            //Re-register the predicates for accepting and acknowledging joins
            register_predicates();
            //But remove the one for start_meta_wedge
            gmsSST.predicates.remove(leader_committed_handle);
        }
        //Construct a predicate that watches for any new committed change
        int curr_num_committed = gmsSST.num_committed[curr_view->rank_of_leader()];
        auto leader_committed_change = [this, curr_num_committed](const DerechoSST& gmsSST) {
            return gmsSST.num_committed[curr_view->rank_of_leader()] > curr_num_committed;
        };
        //Construct a trigger that will re-call finish_view_change() with the same parameters
        auto retry_next_view = [this, next_subgroup_settings, next_num_received_size](DerechoSST& sst) {
            terminate_epoch(next_subgroup_settings, next_num_received_size, sst);
        };
        gmsSST.predicates.insert(leader_committed_change, retry_next_view, sst::PredicateType::ONE_TIME);
        return;
    }
    //If execution reached here, we have a valid next view

    // go through all subgroups first and acknowledge all messages received through SST
    for(const auto& shard_settings_pair : curr_view->multicast_group->get_subgroup_settings()) {
        const subgroup_id_t subgroup_id = shard_settings_pair.first;
        const auto& curr_subgroup_settings = shard_settings_pair.second;
        const auto num_shard_members = curr_subgroup_settings.members.size();
        const std::vector<int>& shard_senders = curr_subgroup_settings.senders;
        auto num_shard_senders = curr_view->multicast_group->get_num_senders(shard_senders);
        std::map<uint32_t, uint32_t> shard_ranks_by_sender_rank;
        for(uint j = 0, l = 0; j < num_shard_members; ++j) {
            if(shard_senders[j]) {
                shard_ranks_by_sender_rank[l] = j;
                l++;
            }
        }
        // wait for all pending sst sends to finish
        while(curr_view->multicast_group->check_pending_sst_sends(subgroup_id)) {
        }
        curr_view->gmsSST->put_with_completion();
        curr_view->gmsSST->sync_with_members();
        while(curr_view->multicast_group->receiver_predicate(subgroup_id, curr_subgroup_settings, shard_ranks_by_sender_rank, num_shard_senders, *curr_view->gmsSST)) {
            auto sst_receive_handler_lambda = [this, subgroup_id, curr_subgroup_settings,
                                               shard_ranks_by_sender_rank,
                                               num_shard_senders](uint32_t sender_rank,
                                                                  volatile char* data, uint32_t size) {
                curr_view->multicast_group->sst_receive_handler(subgroup_id, curr_subgroup_settings,
                                                                shard_ranks_by_sender_rank, num_shard_senders,
                                                                sender_rank, data, size);
            };
            curr_view->multicast_group->receiver_function(subgroup_id, curr_subgroup_settings,
                                                          shard_ranks_by_sender_rank, num_shard_senders, *curr_view->gmsSST,
                                                          curr_view->multicast_group->window_size, sst_receive_handler_lambda);
        }
    }

    curr_view->gmsSST->put_with_completion();
    curr_view->gmsSST->sync_with_members();

    //First, for subgroups in which I'm the shard leader, do RaggedEdgeCleanup for the leader
    auto follower_subgroups_and_shards = std::make_shared<std::map<subgroup_id_t, uint32_t>>();
    for(const auto& subgroup_shard : curr_view->my_subgroups) {
        const subgroup_id_t subgroup_id = subgroup_shard.first;
        const uint32_t shard_num = subgroup_shard.second;
        SubView& shard_view = curr_view->subgroup_shard_views.at(subgroup_id).at(shard_num);
        uint num_shard_senders = 0;
        for(auto v : shard_view.is_sender) {
            if(v) num_shard_senders++;
        }
        if(shard_view.my_rank == curr_view->subview_rank_of_shard_leader(subgroup_id, shard_num)) {
            leader_ragged_edge_cleanup(*curr_view, subgroup_id,
                                       //Ugh, getting the "num_received_offset" out of multicast_group is so ugly
                                       curr_view->multicast_group->get_subgroup_settings()
                                           .at(subgroup_id).num_received_offset,
                                       shard_view.members, num_shard_senders, logger, next_view->members);
        } else {
            //Keep track of which subgroups I'm a non-leader in, and what my corresponding shard ID is
            follower_subgroups_and_shards->emplace(subgroup_id, shard_num);
        }
    }

    //Wait for the shard leaders of subgroups I'm not a leader in to post global_min_ready before continuing
    auto leader_global_mins_are_ready = [this, follower_subgroups_and_shards](const DerechoSST& gmsSST) {
        for(const auto& subgroup_shard_pair : *follower_subgroups_and_shards) {
            SubView& shard_view = curr_view->subgroup_shard_views.at(subgroup_shard_pair.first)
                                          .at(subgroup_shard_pair.second);
            node_id_t shard_leader = shard_view.members.at(curr_view->subview_rank_of_shard_leader(
                    subgroup_shard_pair.first, subgroup_shard_pair.second));
            if(!gmsSST.global_min_ready[curr_view->rank_of(shard_leader)][subgroup_shard_pair.first])
                return false;
        }
        return true;
    };

    auto global_min_ready_continuation = [this, follower_subgroups_and_shards,
                                          next_subgroup_settings, next_num_received_size](DerechoSST& gmsSST) {

        SPDLOG_DEBUG(logger, "GlobalMins are ready for all {} subgroup leaders this node is waiting on", follower_subgroups_and_shards->size());
        //Finish RaggedEdgeCleanup for subgroups in which I'm not the leader
        for(const auto& subgroup_shard_pair : *follower_subgroups_and_shards) {
            const subgroup_id_t subgroup_id = subgroup_shard_pair.first;
            const uint32_t shard_num = subgroup_shard_pair.second;
            SubView& shard_view = curr_view->subgroup_shard_views.at(subgroup_id).at(shard_num);
            uint num_shard_senders = 0;
            for(auto v : shard_view.is_sender) {
                if(v) num_shard_senders++;
            }
            node_id_t shard_leader = shard_view.members[curr_view->subview_rank_of_shard_leader(subgroup_id, shard_num)];
            follower_ragged_edge_cleanup(*curr_view, subgroup_id,
                                         curr_view->rank_of(shard_leader),
                                         curr_view->multicast_group->get_subgroup_settings()
                                                 .at(subgroup_id).num_received_offset,
                                         shard_view.members,
                                         num_shard_senders,
                                         logger);
        }

        //Wait for persistence to finish for messages delivered in RaggedEdgeCleanup
        auto persistence_finished_pred = [this](const DerechoSST& gmsSST) {
            //For each subgroup/shard that this node is a member of...
            for(auto subgroup_shard_pair : curr_view->my_subgroups) {
                const subgroup_id_t subgroup_id = subgroup_shard_pair.first;
                const uint32_t shard_num = subgroup_shard_pair.second;
                if(curr_view->subgroup_shard_views.at(subgroup_id).at(shard_num).mode == Mode::UNORDERED) {
                    //Skip non-ordered subgroups, they never do persistence
                    continue;
                }
                message_id_t last_delivered_seq_num = gmsSST.delivered_num[curr_view->my_rank][subgroup_id];
                //For each member of that shard...
                for(const node_id_t& shard_member : curr_view->subgroup_shard_views.at(subgroup_id).at(shard_num).members) {
                    uint member_row = curr_view->rank_of(shard_member);
                    //Check to see if the member persisted up to the ragged edge trim
                    if(!curr_view->failed[member_row] &&
                            persistent::unpack_version<int32_t>(gmsSST.persisted_num[member_row][subgroup_id]).second < last_delivered_seq_num) {
                        return false;
                    }
                }
            }
            return true;
        };

        auto finish_view_change_trig = [this, follower_subgroups_and_shards,
                                        next_subgroup_settings, next_num_received_size](DerechoSST& gmsSST) {
            finish_view_change(follower_subgroups_and_shards, next_subgroup_settings, next_num_received_size, gmsSST);
        };

        //Last statement in global_min_ready_continuation: register finish_view_change_trig
        gmsSST.predicates.insert(persistence_finished_pred, finish_view_change_trig, sst::PredicateType::ONE_TIME);
    };

    //Last statement in finish_view_change: register global_min_ready_continuation
    gmsSST.predicates.insert(leader_global_mins_are_ready, global_min_ready_continuation, sst::PredicateType::ONE_TIME);
}

void ViewManager::finish_view_change(std::shared_ptr<std::map<subgroup_id_t, uint32_t>> follower_subgroups_and_shards,
                                     std::shared_ptr<std::map<subgroup_id_t, SubgroupSettings>> next_subgroup_settings,
                                     uint32_t next_num_received_size,
                                     DerechoSST& gmsSST) {
    std::unique_lock<std::shared_timed_mutex> write_lock(view_mutex);

    // Disable all the other SST predicates, except suspected_changed
    gmsSST.predicates.remove(start_join_handle);
    gmsSST.predicates.remove(reject_join_handle);
    gmsSST.predicates.remove(change_commit_ready_handle);
    gmsSST.predicates.remove(leader_proposed_handle);

    std::list<tcp::socket> joiner_sockets;
    if(curr_view->i_am_leader() && next_view->joined.size() > 0) {
        //If j joins have been committed, pop the next j sockets off proposed_join_sockets
        //and send them the new View (must happen before we try to do SST setup)
        for(std::size_t c = 0; c < next_view->joined.size(); ++c) {
            commit_join(*next_view, proposed_join_sockets.front());
            //save the socket for later
            joiner_sockets.emplace_back(std::move(proposed_join_sockets.front()));
            proposed_join_sockets.pop_front();
        }
    }

    // Delete the last two GMS predicates from the old SST in preparation for deleting it
    gmsSST.predicates.remove(leader_committed_handle);
    gmsSST.predicates.remove(suspected_changed_handle);

    node_id_t my_id = next_view->members[next_view->my_rank];
    SPDLOG_DEBUG(logger, "Starting creation of new SST and DerechoGroup for view {}", next_view->vid);
    // if new members have joined, add their RDMA connections to SST and RDMC
    for(std::size_t i = 0; i < next_view->joined.size(); ++i) {
        //The new members will be the last joined.size() elements of the members lists
        int joiner_rank = next_view->num_members - next_view->joined.size() + i;
        rdma::impl::verbs_add_connection(next_view->members[joiner_rank], next_view->member_ips[joiner_rank],
                                         my_id);
    }
    for(std::size_t i = 0; i < next_view->joined.size(); ++i) {
        int joiner_rank = next_view->num_members - next_view->joined.size() + i;
        sst::add_node(next_view->members[joiner_rank], next_view->member_ips[joiner_rank]);
    }
    // This will block until everyone responds to SST/RDMC initial handshakes
    transition_multicast_group(*next_subgroup_settings, next_num_received_size);

    // Figure out the IDs of the shard leaders in the old view, then
    // translate the leaders' indices from types to new subgroup IDs
    std::vector<std::vector<int64_t>> old_shard_leaders_by_id =
            translate_types_to_ids(make_shard_leaders_map(*curr_view), *next_view);

    if(curr_view->i_am_leader()) {
        while(!joiner_sockets.empty()) {
            //Send the array of old shard leaders, so the new member knows who to receive from
            std::size_t size_of_vector = mutils::bytes_size(old_shard_leaders_by_id);
            joiner_sockets.front().write(size_of_vector);
            mutils::post_object([&joiner_sockets](const char* bytes, std::size_t size) {
                joiner_sockets.front().write(bytes, size);
            },
                                old_shard_leaders_by_id);
            joiner_sockets.pop_front();
        }
    }

    // New members can now proceed to view_manager.start(), which will call sync()
    next_view->gmsSST->put();
    next_view->gmsSST->sync_with_members();
    SPDLOG_DEBUG(logger, "Done setting up SST and DerechoGroup for view {}", next_view->vid);
    {
        lock_guard_t old_views_lock(old_views_mutex);
        old_views.push(std::move(curr_view));
        old_views_cv.notify_all();
    }
    curr_view = std::move(next_view);

    //Write the new view to disk before using it
    persistent::saveObject(*curr_view);

    //Re-initialize last_suspected (suspected[] has been reset to all false in the new view)
    last_suspected.assign(curr_view->members.size(), false);

    // Register predicates in the new view
    register_predicates();

    // First task with my new view...
    if(curr_view->i_am_new_leader())  // I'm the new leader and everyone who hasn't failed agrees
    {
        curr_view->merge_changes();  // Create a combined list of Changes
    }

    // Announce the new view to the application
    for(auto& view_upcall : view_upcalls) {
        view_upcall(*curr_view);
    }
    // One of those view upcalls is a function in Group that sets up TCP connections to the new members
    // After doing that, shard leaders can send them RPC objects
    send_objects_to_new_members(old_shard_leaders_by_id);

    // Re-initialize this node's RPC objects, which includes receiving them
    // from shard leaders if it is newly a member of a subgroup
    SPDLOG_DEBUG(logger, "Initializing local Replicated Objects");
    initialize_subgroup_objects(my_id, *curr_view, old_shard_leaders_by_id);
    // It's only safe to start evaluating predicates once all RPC objects exist
    curr_view->gmsSST->start_predicate_evaluation();
    view_change_cv.notify_all();
}

/* ------------- 3. Helper Functions for Predicates and Triggers ------------- */

void ViewManager::construct_multicast_group(CallbackSet callbacks,
                                            const DerechoParams& derecho_params,
                                            const std::map<subgroup_id_t, SubgroupSettings>& subgroup_settings,
                                            const uint32_t num_received_size) {
    const auto num_subgroups = curr_view->subgroup_shard_views.size();
    curr_view->gmsSST = std::make_shared<DerechoSST>(
            sst::SSTParams(curr_view->members, curr_view->members[curr_view->my_rank],
                           [this](const uint32_t node_id) { report_failure(node_id); }, curr_view->failed, false),
            num_subgroups, num_received_size, derecho_params.window_size);

    curr_view->multicast_group = std::make_unique<MulticastGroup>(
            curr_view->members, curr_view->members[curr_view->my_rank],
            curr_view->gmsSST, callbacks, num_subgroups,
            subgroup_settings,
            derecho_params,
            persistence_manager_callbacks,
            curr_view->failed);
}

void ViewManager::transition_multicast_group(const std::map<subgroup_id_t, SubgroupSettings>& new_subgroup_settings,
                                             const uint32_t new_num_received_size) {
    const auto num_subgroups = next_view->subgroup_shard_views.size();
    next_view->gmsSST = std::make_shared<DerechoSST>(
            sst::SSTParams(next_view->members, next_view->members[next_view->my_rank],
                           [this](const uint32_t node_id) { report_failure(node_id); }, next_view->failed, false),
            num_subgroups, new_num_received_size, derecho_params.window_size);

    next_view->multicast_group = std::make_unique<MulticastGroup>(
            next_view->members, next_view->members[next_view->my_rank], next_view->gmsSST,
            std::move(*curr_view->multicast_group), num_subgroups,
            new_subgroup_settings,
            persistence_manager_callbacks,
            next_view->failed);

    curr_view->multicast_group.reset();

    // Initialize this node's row in the new SST
    int changes_installed = next_view->joined.size() + next_view->departed.size();
    next_view->gmsSST->init_local_row_from_previous((*curr_view->gmsSST), curr_view->my_rank, changes_installed);
    gmssst::set(next_view->gmsSST->vid[next_view->my_rank], next_view->vid);
}

bool ViewManager::receive_join(tcp::socket& client_socket) {
    DerechoSST& gmsSST = *curr_view->gmsSST;
    if((gmsSST.num_changes[curr_view->my_rank] - gmsSST.num_committed[curr_view->my_rank]) == (int)gmsSST.changes.size()) {
        //TODO: this shouldn't throw an exception, it should just block the client until the group stabilizes
        throw derecho_exception("Too many changes to allow a Join right now");
    }

    struct in_addr joiner_ip_packed;
    inet_aton(client_socket.get_remote_ip().c_str(), &joiner_ip_packed);

    node_id_t joining_client_id = 0;
    client_socket.read(joining_client_id);
    //Safety check: The joiner's ID can't be the same as an existing member's ID, otherwise that member will get kicked out
    if(curr_view->rank_of(joining_client_id) != -1) {
        logger->warn("Joining node at IP {} announced it has ID {}, which is already in the View!", client_socket.get_remote_ip(), joining_client_id);
        client_socket.write(JoinResponse{JoinResponseCode::ID_IN_USE, curr_view->members[curr_view->my_rank]});
        return false;
    }
    client_socket.write(JoinResponse{JoinResponseCode::OK, curr_view->members[curr_view->my_rank]});


    SPDLOG_DEBUG(logger, "Proposing change to add node {}", joining_client_id);
    size_t next_change = gmsSST.num_changes[curr_view->my_rank] - gmsSST.num_installed[curr_view->my_rank];
    gmssst::set(gmsSST.changes[curr_view->my_rank][next_change], joining_client_id);
    gmssst::set(gmsSST.joiner_ips[curr_view->my_rank][next_change], joiner_ip_packed.s_addr);

    gmssst::increment(gmsSST.num_changes[curr_view->my_rank]);

    SPDLOG_DEBUG(logger, "Wedging view {}", curr_view->vid);
    curr_view->wedge();
    SPDLOG_DEBUG(logger, "Leader done wedging view.");
    gmsSST.put(gmsSST.changes.get_base() - gmsSST.getBaseAddress(), gmsSST.num_committed.get_base() - gmsSST.changes.get_base());
    return true;
}

void ViewManager::commit_join(const View& new_view, tcp::socket& client_socket) {
    SPDLOG_DEBUG(logger, "Sending client the new view");
    node_id_t joining_client_id = 0;
    //Extra ID exchange, to match the protocol in await_first_view
    //Optional: Check the result of this exchange to see if the client has crashed while waiting to join
    client_socket.exchange(curr_view->members[curr_view->my_rank], joining_client_id);
    auto bind_socket_write = [&client_socket](const char* bytes, std::size_t size) { client_socket.write(bytes, size); };
    StreamlinedView view_memento(new_view);
    std::size_t size_of_view = mutils::bytes_size(view_memento);
    client_socket.write(size_of_view);
    mutils::post_object(bind_socket_write, view_memento);
    std::size_t size_of_derecho_params = mutils::bytes_size(derecho_params);
    client_socket.write(size_of_derecho_params);
    mutils::post_object(bind_socket_write, derecho_params);
}

void ViewManager::send_objects_to_new_members(const std::vector<std::vector<int64_t>>& old_shard_leaders) {
    node_id_t my_id = curr_view->members[curr_view->my_rank];
    for(subgroup_id_t subgroup_id = 0; subgroup_id < old_shard_leaders.size(); ++subgroup_id) {
        for(uint32_t shard = 0; shard < old_shard_leaders[subgroup_id].size(); ++shard) {
            //if I was the leader of the shard in the old view...
            if(my_id == old_shard_leaders[subgroup_id][shard]) {
                //send its object state to the new members
                for(node_id_t shard_joiner : curr_view->subgroup_shard_views[subgroup_id][shard].joined) {
                    if(shard_joiner != my_id) {
                        send_subgroup_object(subgroup_id, shard_joiner);
                    }
                }
            }
        }
    }
}

/* Note for the future: Since this "send" requires first receiving the log tail length,
 * it's really a blocking receive-then-send. Since all nodes call send_subgroup_object
 * before initialize_subgroup_objects, there's a small chance of a deadlock: node A could
 * be attempting to send an object to node B at the same time as B is attempting to send a
 * different object to A, and neither node will be able to send the log tail length that
 * the other one is waiting on. */
void ViewManager::send_subgroup_object(subgroup_id_t subgroup_id, node_id_t new_node_id) {
    LockedReference<std::unique_lock<std::mutex>, tcp::socket> joiner_socket = group_member_sockets->get_socket(new_node_id);
    //First, read the log tail length sent by the joining node
    int64_t persistent_log_length = 0;
    joiner_socket.get().read(persistent_log_length);
    PersistentRegistry::setEarliestVersionToSerialize(persistent_log_length);
    SPDLOG_DEBUG(logger, "Got log tail length {}", persistent_log_length);
    SPDLOG_DEBUG(logger, "Sending Replicated Object state for subgroup {} to node {}", subgroup_id, new_node_id);
    subgroup_objects.at(subgroup_id).get().send_object(joiner_socket.get());
}

uint32_t ViewManager::compute_num_received_size(const View& view) {
    uint32_t num_received_size = 0;
    for(subgroup_id_t subgroup_num = 0; subgroup_num < view.subgroup_shard_views.size(); ++subgroup_num) {
        uint32_t max_shard_senders = 0;
        for(uint32_t shard_num = 0; shard_num < view.subgroup_shard_views[subgroup_num].size(); ++shard_num) {
            std::size_t shard_size = view.subgroup_shard_views[subgroup_num][shard_num].members.size();
            uint32_t num_shard_senders = view.subgroup_shard_views[subgroup_num][shard_num].num_senders();
            if(num_shard_senders > max_shard_senders) {
                max_shard_senders = shard_size;
            }
        }
        num_received_size += max_shard_senders;
    }
    return num_received_size;
}

uint32_t ViewManager::make_subgroup_maps(const std::unique_ptr<View>& prev_view,
                                         View& curr_view,
                                         std::map<subgroup_id_t, SubgroupSettings>& subgroup_settings) {
    uint32_t num_received_offset = 0;
    int32_t initial_next_unassigned_rank = curr_view.next_unassigned_rank;
    curr_view.subgroup_shard_views.clear();
    curr_view.subgroup_ids_by_type.clear();
    for(const auto& subgroup_type : subgroup_info.membership_function_order) {
        subgroup_shard_layout_t subgroup_shard_views;
        //This is the only place the subgroup membership functions are called; the results are then saved in the View
        try {
            auto temp = subgroup_info.subgroup_membership_functions.at(subgroup_type)(curr_view, curr_view.next_unassigned_rank);
            //Hack to ensure RVO still works even though subgroup_shard_views had to be declared outside this scope
            subgroup_shard_views = std::move(temp);
        } catch(subgroup_provisioning_exception& ex) {
            //Mark the view as inadequate and roll back everything done by previous allocation functions
            curr_view.is_adequately_provisioned = false;
            curr_view.next_unassigned_rank = initial_next_unassigned_rank;
            curr_view.subgroup_shard_views.clear();
            curr_view.subgroup_ids_by_type.clear();

            subgroup_settings.clear();
            return 0;
        }
        std::size_t num_subgroups = subgroup_shard_views.size();
        curr_view.subgroup_ids_by_type[subgroup_type] = std::vector<subgroup_id_t>(num_subgroups);
        for(uint32_t subgroup_index = 0; subgroup_index < num_subgroups; ++subgroup_index) {
            //Assign this (type, index) pair a new unique subgroup ID
            subgroup_id_t curr_subgroup_num = curr_view.subgroup_shard_views.size();
            curr_view.subgroup_ids_by_type[subgroup_type][subgroup_index] = curr_subgroup_num;
            uint32_t num_shards = subgroup_shard_views.at(subgroup_index).size();
            uint32_t max_shard_senders = 0;
            for(uint32_t shard_num = 0; shard_num < num_shards; ++shard_num) {
                SubView& shard_view = subgroup_shard_views.at(subgroup_index).at(shard_num);
                std::size_t shard_size = shard_view.members.size();
                uint32_t num_shard_senders = shard_view.num_senders();
                if(num_shard_senders > max_shard_senders) {
                    max_shard_senders = shard_size; //really? why not max_shard_senders = num_shard_senders?
                }
                //Initialize my_rank in the SubView for this node's ID
                shard_view.my_rank = shard_view.rank_of(curr_view.members[curr_view.my_rank]);
                if(shard_view.my_rank != -1) {
                    curr_view.my_subgroups[curr_subgroup_num] = shard_num;
                    //Save the settings for MulticastGroup
                    subgroup_settings[curr_subgroup_num] = {
                            shard_num,
                            (uint32_t)shard_view.my_rank,
                            shard_view.members,
                            shard_view.is_sender,
                            shard_view.sender_rank_of(shard_view.my_rank),
                            num_received_offset,
                            shard_view.mode};
                }
                if(prev_view) {
                    //Initialize this shard's SubView.joined and SubView.departed
                    subgroup_id_t prev_subgroup_id = prev_view->subgroup_ids_by_type
                                                             .at(subgroup_type)
                                                             .at(subgroup_index);
                    SubView& prev_shard_view = prev_view->subgroup_shard_views[prev_subgroup_id][shard_num];
                    std::set<node_id_t> prev_members(prev_shard_view.members.begin(), prev_shard_view.members.end());
                    std::set<node_id_t> curr_members(shard_view.members.begin(), shard_view.members.end());
                    std::set_difference(curr_members.begin(), curr_members.end(),
                                        prev_members.begin(), prev_members.end(),
                                        std::back_inserter(shard_view.joined));
                    std::set_difference(prev_members.begin(), prev_members.end(),
                                        curr_members.begin(), curr_members.end(),
                                        std::back_inserter(shard_view.departed));
                }
            }
            /* Pull the shard->SubView mapping out of the subgroup membership list
             * and save it under its subgroup ID (which was shard_views_by_subgroup.size()) */
            curr_view.subgroup_shard_views.emplace_back(
                    std::move(subgroup_shard_views[subgroup_index]));
            num_received_offset += max_shard_senders;
        }
    }
    return num_received_offset;
}

std::unique_ptr<View> ViewManager::make_next_view(const std::unique_ptr<View>& curr_view,
                                                  const std::vector<node_id_t>& joiner_ids,
                                                  const std::vector<ip_addr>& joiner_ips,
                                                  std::shared_ptr<spdlog::logger> logger) {
    int next_num_members = curr_view->num_members - curr_view->num_failed + joiner_ids.size();
    std::vector<node_id_t> members(next_num_members), departed;
    std::vector<char> failed(next_num_members);
    std::vector<ip_addr> member_ips(next_num_members);
    int next_unassigned_rank = curr_view->next_unassigned_rank;
    std::set<int> leave_ranks;
    for(std::size_t rank = 0; rank < curr_view->failed.size(); ++rank) {
        if(curr_view->failed[rank]) {
            leave_ranks.emplace(rank);
        }
    }
    for(std::size_t i = 0; i < joiner_ids.size(); ++i) {
        int new_member_rank = curr_view->num_members - leave_ranks.size() + i;
        members[new_member_rank] = joiner_ids[i];
        member_ips[new_member_rank] = joiner_ips[i];
        SPDLOG_DEBUG(logger, "Restarted next view will add new member with id {}", joiner_ids[i]);
    }
    for(const auto& leaver_rank : leave_ranks) {
        departed.emplace_back(curr_view->members[leaver_rank]);
        //Decrement next_unassigned_rank for every failure, unless the failure wasn't assigned to a subgroup anyway
        if(leaver_rank <= curr_view->next_unassigned_rank) {
            next_unassigned_rank--;
        }
    }
    SPDLOG_DEBUG(logger, "Next view will exclude {} failed members.", leave_ranks.size());
    //Copy member information, excluding the members that have failed
    int new_rank = 0;
    for(int old_rank = 0; old_rank < curr_view->num_members; ++old_rank) {
        //This is why leave_ranks needs to be a set
        if(leave_ranks.find(old_rank) == leave_ranks.end()) {
            members[new_rank] = curr_view->members[old_rank];
            member_ips[new_rank] = curr_view->member_ips[old_rank];
            failed[new_rank] = curr_view->failed[old_rank];
            ++new_rank;
        }
    }

    //Initialize my_rank in next_view
    int32_t my_new_rank = -1;
    node_id_t myID = curr_view->members[curr_view->my_rank];
    for(int i = 0; i < next_num_members; ++i) {
        if(members[i] == myID) {
            my_new_rank = i;
            break;
        }
    }
    if(my_new_rank == -1) {
        logger->flush();
        throw derecho_exception("Recovery leader wasn't in the next view it computed?!?!");
    }

    auto next_view = std::make_unique<View>(curr_view->vid + 1, members, member_ips, failed,
                                            joiner_ids, departed, my_new_rank, next_unassigned_rank);
    next_view->i_know_i_am_leader = curr_view->i_know_i_am_leader;
    return std::move(next_view);
}

std::unique_ptr<View> ViewManager::make_next_view(const std::unique_ptr<View>& curr_view,
                                                  const DerechoSST& gmsSST,
                                                  std::shared_ptr<spdlog::logger> logger) {
    int myRank = curr_view->my_rank;
    std::set<int> leave_ranks;
    std::vector<int> join_indexes;
    //Look through pending changes up to num_committed and filter the joins and leaves
    const int committed_count = gmsSST.num_committed[curr_view->rank_of_leader()]
                                - gmsSST.num_installed[curr_view->rank_of_leader()];
    for(int change_index = 0; change_index < committed_count; change_index++) {
        node_id_t change_id = gmsSST.changes[myRank][change_index];
        int change_rank = curr_view->rank_of(change_id);
        if(change_rank != -1) {
            //Might as well save the rank, since we'll need it again
            leave_ranks.emplace(change_rank);
        } else {
            join_indexes.emplace_back(change_index);
        }
    }

    int next_num_members = curr_view->num_members - leave_ranks.size()
                           + join_indexes.size();
    //Initialize the next view
    std::vector<node_id_t> joined, members(next_num_members), departed;
    std::vector<char> failed(next_num_members);
    std::vector<ip_addr> member_ips(next_num_members);
    int next_unassigned_rank = curr_view->next_unassigned_rank;
    for(std::size_t i = 0; i < join_indexes.size(); ++i) {
        const int join_index = join_indexes[i];
        node_id_t joiner_id = gmsSST.changes[myRank][join_index];
        struct in_addr joiner_ip_packed;
        joiner_ip_packed.s_addr = gmsSST.joiner_ips[myRank][join_index];
        char* joiner_ip_cstr = inet_ntoa(joiner_ip_packed);
        std::string joiner_ip(joiner_ip_cstr);

        joined.emplace_back(joiner_id);
        //New members go at the end of the members list, but it may shrink in the new view
        int new_member_rank = curr_view->num_members - leave_ranks.size() + i;
        members[new_member_rank] = joiner_id;
        member_ips[new_member_rank] = joiner_ip;
        SPDLOG_DEBUG(logger, "Next view will add new member with ID {}", joiner_id);
    }
    for(const auto& leaver_rank : leave_ranks) {
        departed.emplace_back(curr_view->members[leaver_rank]);
        //Decrement next_unassigned_rank for every failure, unless the failure wasn't assigned to a subgroup anyway
        if(leaver_rank <= curr_view->next_unassigned_rank) {
            next_unassigned_rank--;
        }
    }
    SPDLOG_DEBUG(logger, "Next view will exclude {} failed members.", leave_ranks.size());

    //Copy member information, excluding the members that have failed
    int new_rank = 0;
    for(int old_rank = 0; old_rank < curr_view->num_members; ++old_rank) {
        //This is why leave_ranks needs to be a set
        if(leave_ranks.find(old_rank) == leave_ranks.end()) {
            members[new_rank] = curr_view->members[old_rank];
            member_ips[new_rank] = curr_view->member_ips[old_rank];
            failed[new_rank] = curr_view->failed[old_rank];
            ++new_rank;
        }
    }

    //Initialize my_rank in next_view
    int32_t my_new_rank = -1;
    node_id_t myID = curr_view->members[myRank];
    for(int i = 0; i < next_num_members; ++i) {
        if(members[i] == myID) {
            my_new_rank = i;
            break;
        }
    }
    if(my_new_rank == -1) {
        throw derecho_exception("Some other node reported that I failed.  Node " + std::to_string(myID) + " terminating");
    }

    auto next_view = std::make_unique<View>(curr_view->vid + 1, members, member_ips, failed,
                                            joined, departed, my_new_rank, next_unassigned_rank);
    next_view->i_know_i_am_leader = curr_view->i_know_i_am_leader;
    return std::move(next_view);
}

std::map<std::type_index, std::vector<std::vector<int64_t>>> ViewManager::make_shard_leaders_map(const View& view) {
    std::map<std::type_index, std::vector<std::vector<int64_t>>> shard_leaders_by_type;
    for(const auto& type_to_ids : view.subgroup_ids_by_type) {
        //Raw subgroups don't have any state to send to new members
        if(type_to_ids.first == std::type_index(typeid(RawObject))) {
            continue;
        }
        shard_leaders_by_type[type_to_ids.first].resize(type_to_ids.second.size());
        for(uint32_t subgroup_index = 0; subgroup_index < type_to_ids.second.size(); ++subgroup_index) {
            subgroup_id_t subgroup_id = type_to_ids.second[subgroup_index];
            std::size_t num_shards = view.subgroup_shard_views[subgroup_id].size();
            shard_leaders_by_type[type_to_ids.first][subgroup_index].resize(num_shards, -1);
            for(uint32_t shard = 0; shard < num_shards; ++shard) {
                int shard_leader_rank = view.subview_rank_of_shard_leader(subgroup_id, shard);
                if(shard_leader_rank >= 0) {
                    shard_leaders_by_type[type_to_ids.first][subgroup_index][shard]
                            = view.subgroup_shard_views[subgroup_id][shard].members[shard_leader_rank];
                }
            }
        }
    }
    return shard_leaders_by_type;
}

std::vector<std::vector<int64_t>> ViewManager::translate_types_to_ids(
        const std::map<std::type_index, std::vector<std::vector<int64_t>>>& old_shard_leaders_by_type,
        const View& new_view) {
    std::vector<std::vector<int64_t>> old_shard_leaders_by_id(new_view.subgroup_shard_views.size());
    assert(new_view.is_adequately_provisioned);
    for(const auto& type_vector_pair : old_shard_leaders_by_type) {
        const auto& leaders_by_index_and_shard = type_vector_pair.second;
        for(std::size_t subgroup_index = 0; subgroup_index < leaders_by_index_and_shard.size(); ++subgroup_index) {
            subgroup_id_t new_subgroup_id = new_view.subgroup_ids_by_type.at(type_vector_pair.first)
                                                    .at(subgroup_index);
            std::size_t num_shards = leaders_by_index_and_shard[subgroup_index].size();
            old_shard_leaders_by_id[new_subgroup_id].resize(num_shards, -1);
            for(std::size_t shard = 0; shard < num_shards; ++shard) {
                old_shard_leaders_by_id[new_subgroup_id][shard]
                        = leaders_by_index_and_shard[subgroup_index][shard];
            }
        }
    }
    return old_shard_leaders_by_id;
}

bool ViewManager::suspected_not_equal(const DerechoSST& gmsSST, const std::vector<bool>& old) {
    for(unsigned int r = 0; r < gmsSST.get_num_rows(); r++) {
        for(size_t who = 0; who < gmsSST.suspected.size(); who++) {
            if(gmsSST.suspected[r][who] && !old[who]) {
                return true;
            }
        }
    }
    return false;
}

void ViewManager::copy_suspected(const DerechoSST& gmsSST, std::vector<bool>& old) {
    for(size_t who = 0; who < gmsSST.suspected.size(); ++who) {
        old[who] = gmsSST.suspected[gmsSST.get_local_index()][who];
    }
}

bool ViewManager::changes_contains(const DerechoSST& gmsSST, const node_id_t q) {
    int myRow = gmsSST.get_local_index();
    for(int p_index = 0; p_index < gmsSST.num_changes[myRow] - gmsSST.num_installed[myRow]; p_index++) {
        const node_id_t p(const_cast<node_id_t&>(gmsSST.changes[myRow][p_index]));
        if(p == q) {
            return true;
        }
    }
    return false;
}

int ViewManager::min_acked(const DerechoSST& gmsSST, const std::vector<char>& failed) {
    int myRank = gmsSST.get_local_index();
    int min = gmsSST.num_acked[myRank];
    for(size_t n = 0; n < failed.size(); n++) {
        if(!failed[n] && gmsSST.num_acked[n] < min) {
            min = gmsSST.num_acked[n];
        }
    }

    return min;
}

void ViewManager::deliver_in_order(const View& Vc, const int shard_leader_rank,
                                   const uint32_t subgroup_num, const uint32_t num_received_offset,
                                   const std::vector<node_id_t>& shard_members, uint num_shard_senders,
                                   std::shared_ptr<spdlog::logger> logger) {
    // Ragged cleanup is finished, deliver in the implied order
    std::vector<int32_t> max_received_indices(num_shard_senders);
    std::stringstream delivery_order;
    for(uint n = 0; n < num_shard_senders; n++) {
        if(logger->should_log(spdlog::level::debug)) {
            delivery_order << "Subgroup " << subgroup_num
                    << " " << Vc.members[Vc.my_rank]
                    << ":0.."
                    << Vc.gmsSST->global_min[shard_leader_rank][num_received_offset + n]
                    << " ";
        }
        max_received_indices[n] = Vc.gmsSST->global_min[shard_leader_rank][num_received_offset + n];
    }
    std::string log_file_name = "RaggedTrim_" + std::to_string(subgroup_num);
    RaggedTrim trim_log{subgroup_num, Vc.vid, Vc.members[Vc.rank_of_leader()], max_received_indices};
    SPDLOG_DEBUG(logger, "Logging ragged trim to disk");
    persistent::saveObject(trim_log, log_file_name.c_str());
    SPDLOG_DEBUG(logger, "Delivering ragged-edge messages in order: {}", delivery_order.str());
    Vc.multicast_group->deliver_messages_upto(max_received_indices, subgroup_num, num_shard_senders);
}

void ViewManager::leader_ragged_edge_cleanup(View& Vc, const subgroup_id_t subgroup_num,
                                             const uint32_t num_received_offset,
                                             const std::vector<node_id_t>& shard_members, uint num_shard_senders,
                                             std::shared_ptr<spdlog::logger> logger, const std::vector<node_id_t>& next_view_members) {
    SPDLOG_DEBUG(logger, "Running leader RaggedEdgeCleanup for subgroup {}", subgroup_num);
    int myRank = Vc.my_rank;
    // int Leader = Vc.rank_of_leader();  // We don't want this to change under our feet
    bool found = false;
    for(uint n = 0; n < shard_members.size() && !found; n++) {
        const auto node_id = shard_members[n];
        const auto node_rank = Vc.rank_of(node_id);
        if(Vc.gmsSST->global_min_ready[node_rank][subgroup_num]) {
            gmssst::set(Vc.gmsSST->global_min[myRank] + num_received_offset,
                        Vc.gmsSST->global_min[node_rank] + num_received_offset, num_shard_senders);
            found = true;
        }
    }

    if(!found) {
        for(uint n = 0; n < num_shard_senders; n++) {
            int min = Vc.gmsSST->num_received[myRank][num_received_offset + n];
            for(uint r = 0; r < shard_members.size(); r++) {
                const auto node_id = shard_members[r];
                const auto node_rank = Vc.rank_of(node_id);
                if(!Vc.failed[node_rank] && min > Vc.gmsSST->num_received[node_rank][num_received_offset + n]) {
                    min = Vc.gmsSST->num_received[node_rank][num_received_offset + n];
                }
            }

            gmssst::set(Vc.gmsSST->global_min[myRank][num_received_offset + n], min);
        }
    }

    SPDLOG_DEBUG(logger, "Shard leader for subgroup {} finished computing global_min", subgroup_num);
    gmssst::set(Vc.gmsSST->global_min_ready[myRank][subgroup_num], true);
    Vc.gmsSST->put(Vc.multicast_group->get_shard_sst_indices(subgroup_num),
                   (char*)std::addressof(Vc.gmsSST->global_min[0][num_received_offset]) - Vc.gmsSST->getBaseAddress(),
                   sizeof(Vc.gmsSST->global_min[0][num_received_offset]) * num_shard_senders);
    Vc.gmsSST->put(Vc.multicast_group->get_shard_sst_indices(subgroup_num),
                   (char*)std::addressof(Vc.gmsSST->global_min_ready[0][subgroup_num]) - Vc.gmsSST->getBaseAddress(),
                   sizeof(Vc.gmsSST->global_min_ready[0][subgroup_num]));

    deliver_in_order(Vc, myRank, subgroup_num, num_received_offset, shard_members, num_shard_senders, logger);
    SPDLOG_DEBUG(logger, "Done with RaggedEdgeCleanup for subgroup {}", subgroup_num);
}

void ViewManager::follower_ragged_edge_cleanup(View& Vc, const subgroup_id_t subgroup_num,
                                               uint shard_leader_rank,
                                               const uint32_t num_received_offset,
                                               const std::vector<node_id_t>& shard_members, uint num_shard_senders,
                                               std::shared_ptr<spdlog::logger> logger) {
    int myRank = Vc.my_rank;
    // Learn the leader's data and push it before acting upon it
    SPDLOG_DEBUG(logger, "Running follower RaggedEdgeCleanup for subgroup {}; echoing leader's global_min", subgroup_num);
    gmssst::set(Vc.gmsSST->global_min[myRank] + num_received_offset, Vc.gmsSST->global_min[shard_leader_rank] + num_received_offset,
                num_shard_senders);
    gmssst::set(Vc.gmsSST->global_min_ready[myRank][subgroup_num], true);
    Vc.gmsSST->put(Vc.multicast_group->get_shard_sst_indices(subgroup_num),
                   (char*)std::addressof(Vc.gmsSST->global_min[0][num_received_offset]) - Vc.gmsSST->getBaseAddress(),
                   sizeof(Vc.gmsSST->global_min[0][num_received_offset]) * num_shard_senders);
    Vc.gmsSST->put(Vc.multicast_group->get_shard_sst_indices(subgroup_num),
                   (char*)std::addressof(Vc.gmsSST->global_min_ready[0][subgroup_num]) - Vc.gmsSST->getBaseAddress(),
                   sizeof(Vc.gmsSST->global_min_ready[0][subgroup_num]));

    deliver_in_order(Vc, shard_leader_rank, subgroup_num, num_received_offset, shard_members, num_shard_senders, logger);
    SPDLOG_DEBUG(logger, "Done with RaggedEdgeCleanup for subgroup {}", subgroup_num);
}

/* ------------- 4. Public-Interface methods of ViewManager ------------- */

void ViewManager::report_failure(const node_id_t who) {
    int r = curr_view->rank_of(who);
    SPDLOG_DEBUG(logger, "Node ID {} failure reported; marking suspected[{}]", who, r);
    curr_view->gmsSST->suspected[curr_view->my_rank][r] = true;
    int cnt = 0;
    for(r = 0; r < (int)curr_view->gmsSST->suspected.size(); r++) {
        if(curr_view->gmsSST->suspected[curr_view->my_rank][r]) {
            ++cnt;
        }
    }

    if(cnt >= (curr_view->num_members + 1) / 2) {
        logger->flush();
        throw derecho_exception("Potential partitioning event: this node is no longer in the majority and must shut down!");
    }
    curr_view->gmsSST->put((char*)std::addressof(curr_view->gmsSST->suspected[0][r]) - curr_view->gmsSST->getBaseAddress(), sizeof(curr_view->gmsSST->suspected[0][r]));
}

void ViewManager::leave() {
    shared_lock_t lock(view_mutex);
    SPDLOG_DEBUG(logger, "Cleanly leaving the group.");
    curr_view->multicast_group->wedge();
    curr_view->gmsSST->predicates.clear();
    curr_view->gmsSST->suspected[curr_view->my_rank][curr_view->my_rank] = true;
    curr_view->gmsSST->put((char*)std::addressof(curr_view->gmsSST->suspected[0][curr_view->my_rank]) - curr_view->gmsSST->getBaseAddress(), sizeof(curr_view->gmsSST->suspected[0][curr_view->my_rank]));
    thread_shutdown = true;
}

char* ViewManager::get_sendbuffer_ptr(subgroup_id_t subgroup_num, unsigned long long int payload_size,
                                      int pause_sending_turns,
                                      bool cooked_send, bool null_send) {
    shared_lock_t lock(view_mutex);
    return curr_view->multicast_group->get_sendbuffer_ptr(subgroup_num, payload_size, pause_sending_turns, cooked_send, null_send);
}

void ViewManager::send(subgroup_id_t subgroup_num) {
    shared_lock_t lock(view_mutex);
    view_change_cv.wait(lock, [&]() {
        return curr_view->multicast_group->send(subgroup_num);
    });
}

const uint64_t ViewManager::compute_global_stability_frontier(subgroup_id_t subgroup_num) {
    shared_lock_t lock(view_mutex);
    return curr_view->multicast_group->compute_global_stability_frontier(subgroup_num);
}

void ViewManager::add_view_upcall(const view_upcall_t& upcall) {
    view_upcalls.emplace_back(upcall);
}

std::vector<node_id_t> ViewManager::get_members() {
    shared_lock_t read_lock(view_mutex);
    return curr_view->members;
}

void ViewManager::barrier_sync() {
    shared_lock_t read_lock(view_mutex);
    curr_view->gmsSST->sync_with_members();
}

SharedLockedReference<View> ViewManager::get_current_view() {
    return SharedLockedReference<View>(*curr_view, view_mutex);
}

void ViewManager::debug_print_status() const {
    std::cout << "curr_view = " << curr_view->debug_string() << std::endl;
}

//void ViewManager::print_log(std::ostream& output_dest) const {
//    for(size_t i = 0; i < util::debug_log().curr_event; ++i) {
//        output_dest << util::debug_log().times[i] << "," << util::debug_log().events[i] << "," << std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ")[curr_view->members[curr_view->my_rank]] << std::endl;
//    }
//}

} /* namespace derecho */
