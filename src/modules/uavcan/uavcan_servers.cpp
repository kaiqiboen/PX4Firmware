/****************************************************************************
 *
 *   Copyright (c) 2014 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <systemlib/err.h>
#include <systemlib/systemlib.h>
#include <systemlib/param/param.h>
#include <systemlib/mixer/mixer.h>
#include <systemlib/board_serial.h>
#include <systemlib/scheduling_priorities.h>
#include <systemlib/git_version.h>
#include <version/version.h>
#include <arch/board/board.h>
#include <arch/chip/chip.h>

#include "uavcan_main.hpp"
#include "uavcan_servers.hpp"
#include "uavcan_virtual_can_driver.hpp"

#include <uavcan_posix/dynamic_node_id_server/file_event_tracer.hpp>
#include <uavcan_posix/dynamic_node_id_server/file_storage_backend.hpp>
#include <uavcan_posix/firmware_version_checker.hpp>

#include <uORB/topics/vehicle_command.h>
#include <uORB/topics/uavcan_parameter_request.h>
#include <uORB/topics/uavcan_parameter_value.h>

#include <mavlink/v1.0/common/mavlink.h>

ORB_DEFINE(uavcan_parameter_request, struct uavcan_parameter_request_s);
ORB_DEFINE(uavcan_parameter_value, struct uavcan_parameter_value_s);

//todo:The Inclusion of file_server_backend is killing
// #include <sys/types.h> and leaving OK undefined
# define OK 0



/**
 * @file uavcan_servers.cpp
 *
 * Implements basic functionality of UAVCAN node.
 *
 * @author Pavel Kirienko <pavel.kirienko@gmail.com>
 *         David Sidrane <david_s5@nscdg.com>
 */

/*
 * UavcanNode
 */
UavcanServers *UavcanServers::_instance;
UavcanServers::UavcanServers(uavcan::INode &main_node) :
	_subnode_thread(-1),
	_vdriver(NumIfaces, uavcan_stm32::SystemClock::instance()),
	_subnode(_vdriver, uavcan_stm32::SystemClock::instance()),
	_main_node(main_node),
	_tracer(),
	_storage_backend(),
	_fw_version_checker(),
	_server_instance(_subnode, _storage_backend, _tracer),
	_fileserver_backend(_subnode),
	_node_info_retriever(_subnode),
	_fw_upgrade_trigger(_subnode, _fw_version_checker),
	_fw_server(_subnode, _fileserver_backend),
	_count_in_progress(false),
	_count_index(0),
	_param_in_progress(0),
	_param_index(0),
	_param_list_in_progress(false),
	_param_list_all_nodes(false),
	_param_list_node_id(1),
	_cmd_in_progress(false),
	_param_response_pub(nullptr),
	_param_getset_client(_subnode),
	_mutex_inited(false),
	_check_fw(false),
	_esc_enumeration_active(false),
	_esc_enumeration_index(0),
	_beep_pub(_subnode),
	_enumeration_indication_sub(_subnode),
	_enumeration_client(_subnode),
	_enumeration_getset_client(_subnode),
	_enumeration_save_client(_subnode)

{
}

UavcanServers::~UavcanServers()
{
	if (_mutex_inited) {
		(void)Lock::deinit(_subnode_mutex);
	}

	_main_node.getDispatcher().removeRxFrameListener();
}

int UavcanServers::stop()
{
	UavcanServers *server = instance();

	if (server == nullptr) {
		warnx("Already stopped");
		return -1;
	}

	_instance = nullptr;

	if (server->_subnode_thread != -1) {
		pthread_cancel(server->_subnode_thread);
		pthread_join(server->_subnode_thread, NULL);
	}

	server->_main_node.getDispatcher().removeRxFrameListener();

	delete server;
	return 0;
}

int UavcanServers::start(uavcan::INode &main_node)
{
	if (_instance != nullptr) {
		warnx("Already started");
		return -1;
	}

	/*
	 * Node init
	 */
	_instance = new UavcanServers(main_node);

	if (_instance == nullptr) {
		warnx("Out of memory");
		return -2;
	}

	int rv  = _instance->init();

	if (rv < 0) {
		warnx("Node init failed: %d", rv);
		delete _instance;
		_instance = nullptr;
		return rv;
	}

	/*
	 * Start the thread. Normally it should never exit.
	 */
	pthread_attr_t tattr;
	struct sched_param param;

	pthread_attr_init(&tattr);
	tattr.stacksize = StackSize;
	param.sched_priority = Priority;
	pthread_attr_setschedparam(&tattr, &param);

	static auto run_trampoline = [](void *) {return UavcanServers::_instance->run(_instance);};

	rv = pthread_create(&_instance->_subnode_thread, &tattr, static_cast<pthread_startroutine_t>(run_trampoline), NULL);

	if (rv != 0) {
		rv = -rv;
		warnx("pthread_create() failed: %d", rv);
		delete _instance;
		_instance = nullptr;
	}

	return rv;
}

int UavcanServers::init()
{
	errno = 0;

	/*
	 * Initialize the mutex.
	 * giving it its path
	 */

	int ret = Lock::init(_subnode_mutex);

	if (ret < 0) {
		warnx("Lock init: %d", errno);
		return ret;
	}

	_mutex_inited = true;

	_subnode.setNodeID(_main_node.getNodeID());
	_main_node.getDispatcher().installRxFrameListener(&_vdriver);


	/*
	 * Initialize the fw version checker.
	 * giving it its path
	 */
	ret = _fw_version_checker.createFwPaths(UAVCAN_FIRMWARE_PATH);

	if (ret < 0) {
		warnx("FirmwareVersionChecker init: %d, errno: %d", ret, errno);
		return ret;
	}

	/* Start fw file server back */

	ret = _fw_server.start();

	if (ret < 0) {
		warnx("BasicFileServer init: %d, errno: %d", ret, errno);
		return ret;
	}

	/* Initialize storage back end for the node allocator using UAVCAN_NODE_DB_PATH directory */

	ret = _storage_backend.init(UAVCAN_NODE_DB_PATH);

	if (ret < 0) {
		warnx("FileStorageBackend init: %d, errno: %d", ret, errno);
		return ret;
	}

	/* Initialize trace in the UAVCAN_NODE_DB_PATH directory */

	ret = _tracer.init(UAVCAN_LOG_FILE);

	if (ret < 0) {
		warnx("FileEventTracer init: %d, errno: %d", ret, errno);
		return ret;
	}

	/* hardware version */
	uavcan::protocol::HardwareVersion hwver;
	UavcanNode::getHardwareVersion(hwver);

	/* Initialize the dynamic node id server  */
	ret = _server_instance.init(hwver.unique_id);

	if (ret < 0) {
		warnx("CentralizedServer init: %d", ret);
		return ret;
	}

	/* Start node info retriever to fetch node info from new nodes */

	ret = _node_info_retriever.start();

	if (ret < 0) {
		warnx("NodeInfoRetriever init: %d", ret);
		return ret;
	}

	/* Start the fw version checker   */

	ret = _fw_upgrade_trigger.start(_node_info_retriever, _fw_version_checker.getFirmwarePath());

	if (ret < 0) {
		warnx("FirmwareUpdateTrigger init: %d", ret);
		return ret;
	}

	/*  Start the Node   */

	return OK;
}

pthread_addr_t UavcanServers::run(pthread_addr_t)
{
	prctl(PR_SET_NAME, "uavcan fw srv", 0);

	Lock lock(_subnode_mutex);

	/* the subscribe call needs to happen in the same thread,
	 * so not in the constructor */
	int cmd_sub = orb_subscribe(ORB_ID(vehicle_command));
	int param_request_sub = orb_subscribe(ORB_ID(uavcan_parameter_request));
	int armed_sub = orb_subscribe(ORB_ID(actuator_armed));

	/* Set up shared service clients */
	_param_getset_client.setCallback(GetSetCallback(this, &UavcanServers::cb_getset));
	_enumeration_client.setCallback(EnumerationBeginCallback(this, &UavcanServers::cb_enumeration_begin));
	_enumeration_indication_sub.start(EnumerationIndicationCallback(this, &UavcanServers::cb_enumeration_indication));
	_enumeration_getset_client.setCallback(GetSetCallback(this, &UavcanServers::cb_enumeration_getset));
	_enumeration_save_client.setCallback(ExecuteOpcodeCallback(this, &UavcanServers::cb_enumeration_save));

	uavcan::ServiceClient<uavcan::protocol::RestartNode, RestartNodeCallback> restartnode_client(_subnode);
	restartnode_client.setCallback(RestartNodeCallback(this, &UavcanServers::cb_restart));

	uavcan::ServiceClient<uavcan::protocol::param::ExecuteOpcode, ExecuteOpcodeCallback> opcode_client(_subnode);
	opcode_client.setCallback(ExecuteOpcodeCallback(this, &UavcanServers::cb_opcode));

	_count_in_progress = _param_in_progress = _param_list_in_progress = _cmd_in_progress = _param_list_all_nodes = false;
	memset(_param_counts, 0, sizeof(_param_counts));

	_esc_enumeration_active = false;
	memset(_esc_enumeration_ids, 0, sizeof(_esc_enumeration_ids));
	_esc_enumeration_index = 0;

	while (1) {

		if (_check_fw == true) {
			_check_fw = false;
			_node_info_retriever.invalidateAll();
		}

		const int spin_res = _subnode.spin(uavcan::MonotonicDuration::fromMSec(10));
		if (spin_res < 0) {
			warnx("node spin error %i", spin_res);
		}

		// Check for parameter requests (get/set/list)
		bool param_request_ready;
		orb_check(param_request_sub, &param_request_ready);

		if (param_request_ready && !_param_list_in_progress && !_param_in_progress && !_count_in_progress) {
			struct uavcan_parameter_request_s request;
			orb_copy(ORB_ID(uavcan_parameter_request), param_request_sub, &request);

			if (_param_counts[request.node_id]) {
				/*
				 * We know how many parameters are exposed by this node, so
				 * process the request.
				 */
				if (request.message_type == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
					uavcan::protocol::param::GetSet::Request req;
					if (request.param_index >= 0) {
						req.index = request.param_index;
					} else {
						req.name = (char*)request.param_id;
					}

					int call_res = _param_getset_client.call(request.node_id, req);
					if (call_res < 0) {
						warnx("UAVCAN command bridge: couldn't send GetSet: %d", call_res);
					} else {
						_param_in_progress = true;
						_param_index = request.param_index;
						warnx("UAVCAN command bridge: sent GetSet");
					}
				} else if (request.message_type == MAVLINK_MSG_ID_PARAM_SET) {
					uavcan::protocol::param::GetSet::Request req;
					if (request.param_index >= 0) {
						req.index = request.param_index;
					} else {
						req.name = (char*)request.param_id;
					}

					if (request.param_type == MAV_PARAM_TYPE_REAL32) {
						req.value.to<uavcan::protocol::param::Value::Tag::real_value>() = request.real_value;
					} else if (request.param_type == MAV_PARAM_TYPE_UINT8) {
						req.value.to<uavcan::protocol::param::Value::Tag::boolean_value>() = request.int_value;
					} else {
						req.value.to<uavcan::protocol::param::Value::Tag::integer_value>() = request.int_value;
					}

					int call_res = _param_getset_client.call(request.node_id, req);
					if (call_res < 0) {
						warnx("UAVCAN command bridge: couldn't send GetSet: %d", call_res);
					} else {
						_param_in_progress = true;
						_param_index = request.param_index;
						warnx("UAVCAN command bridge: sent GetSet");
					}
				} else if (request.message_type == MAVLINK_MSG_ID_PARAM_REQUEST_LIST) {
					// This triggers the _param_list_in_progress case below.
					_param_index = 0;
					_param_list_in_progress = true;
					_param_list_node_id = request.node_id;
					_param_list_all_nodes = false;

					warnx("UAVCAN command bridge: starting component-specific param list");
				}
			} else if (request.node_id == MAV_COMP_ID_ALL) {
				if (request.message_type == MAVLINK_MSG_ID_PARAM_REQUEST_LIST) {
					/*
					 * This triggers the _param_list_in_progress case below,
					 * but additionally iterates over all active nodes.
					 */
					_param_index = 0;
					_param_list_in_progress = true;
					_param_list_node_id = get_next_active_node_id(1);
					_param_list_all_nodes = true;

					warnx("UAVCAN command bridge: starting global param list");

					if (_param_counts[_param_list_node_id.get()] == 0) {
						param_count(_param_list_node_id);
					}
				}
			} else {
				/*
				 * Need to know how many parameters this node has before we can
				 * continue; count them now and then process the request.
				 */
				param_count(request.node_id);
			}
		}

		// Handle parameter listing index/node ID advancement
		if (_param_list_in_progress && !_param_in_progress && !_count_in_progress) {
			if (_param_index >= _param_counts[_param_list_node_id.get()]) {
				// Reached the end of the current node's parameter set.
				_param_list_in_progress = false;

				if (_param_list_all_nodes) {
					// We're listing all parameters for all nodes -- get the next node ID
					uavcan::NodeID next_id = get_next_active_node_id(_param_list_node_id);
					if (next_id != _param_list_node_id) {
						/*
						 * If there is a next node ID, check if that node's parameters
						 * have been counted before. If not, do it now.
						 */
						if (_param_counts[_param_list_node_id.get()] == 0) {
							param_count(_param_list_node_id);
						}
						// Keep on listing.
						_param_index = 0;
						_param_list_in_progress = true;
						warnx("UAVCAN command bridge: incrementing global param list node ID");
					}
				}
			}
		}

		// Check if we're still listing, and need to get the next parameter
		if (_param_list_in_progress && !_param_in_progress && !_count_in_progress) {
			// Ready to request the next value -- _param_index is incremented
			// after each successful fetch by cb_getset
			uavcan::protocol::param::GetSet::Request req;
			req.index = _param_index;

			int call_res = _param_getset_client.call(_param_list_node_id, req);
			if (call_res < 0) {
				_param_list_in_progress = false;
				warnx("UAVCAN command bridge: couldn't send GetSet: %d", call_res);
			} else {
				_param_in_progress = true;
				warnx("UAVCAN command bridge: sent GetSet during param list operation");
			}
		}

		// Check for ESC enumeration commands
		bool cmd_ready;
		orb_check(cmd_sub, &cmd_ready);

		if (cmd_ready && !_cmd_in_progress) {
			struct vehicle_command_s cmd;
			orb_copy(ORB_ID(vehicle_command), cmd_sub, &cmd);

			if (cmd.command == vehicle_command_s::VEHICLE_CMD_PREFLIGHT_UAVCAN) {
				int command_id = static_cast<int>(cmd.param1 + 0.5f);
				int node_id = static_cast<int>(cmd.param2 + 0.5f);
				int call_res;

				warnx("UAVCAN command bridge: received command ID %d, node ID %d", command_id, node_id);

				switch (command_id) {
				case 0:
				case 1: {
						_esc_enumeration_active = command_id;
						_esc_enumeration_index = 0;
						_esc_count = 0;
						uavcan::protocol::enumeration::Begin::Request req;
						req.parameter_name = "esc_index";
						req.timeout_sec = _esc_enumeration_active ? 65535 : 0;
						call_res = _enumeration_client.call(get_next_active_node_id(1), req);
						if (call_res < 0) {
							warnx("UAVCAN ESC enumeration: couldn't send initial Begin request: %d", call_res);
						}
						break;
					}
				case 2: {
						// Command is a restart node request
						uavcan::protocol::RestartNode::Request restart_req;
						restart_req.magic_number = restart_req.MAGIC_NUMBER;
						call_res = restartnode_client.call(node_id, restart_req);
						if (call_res < 0) {
							warnx("UAVCAN command bridge: couldn't send RestartNode: %d", call_res);
						} else {
							_cmd_in_progress = true;
							warnx("UAVCAN command bridge: sent RestartNode");
						}
						break;
					}
				case 3:
				case 4: {
						// Command is a param save or erase request
						uavcan::protocol::param::ExecuteOpcode::Request opcode_req;
						opcode_req.opcode = command_id == 3 ? opcode_req.OPCODE_SAVE : opcode_req.OPCODE_ERASE;
						call_res = opcode_client.call(node_id, opcode_req);
						if (call_res < 0) {
							warnx("UAVCAN command bridge: couldn't send ExecuteOpcode: %d", call_res);
						} else {
							_cmd_in_progress = true;
							warnx("UAVCAN command bridge: sent ExecuteOpcode");
						}
						break;
					}
				default: {
						warnx("UAVCAN command bridge: unknown command ID %d", command_id);
						break;
					}
				}
			}
		}

		// Shut down once armed
		// TODO (elsewhere): start up again once disarmed?
		bool updated;
		orb_check(armed_sub, &updated);
		if (updated) {
			struct actuator_armed_s armed;
			orb_copy(ORB_ID(actuator_armed), armed_sub, &armed);

			if (armed.armed && !armed.lockdown) {
				warnx("UAVCAN command bridge: system armed, exiting now.");
				break;
			}
		}
	}

	warnx("exiting.");
	return (pthread_addr_t) 0;
}

void UavcanServers::cb_restart(const uavcan::ServiceCallResult<uavcan::protocol::RestartNode> &result)
{
	bool success = result.isSuccessful();
	uavcan::protocol::RestartNode::Response resp = result.getResponse();
	success &= resp.ok;
	_cmd_in_progress = false;
}

void UavcanServers::cb_opcode(const uavcan::ServiceCallResult<uavcan::protocol::param::ExecuteOpcode> &result)
{
	bool success = result.isSuccessful();
	uavcan::protocol::param::ExecuteOpcode::Response resp = result.getResponse();
	success &= resp.ok;
	_cmd_in_progress = false;
}

void UavcanServers::cb_getset(const uavcan::ServiceCallResult<uavcan::protocol::param::GetSet> &result)
{
	if (_count_in_progress) {
		/*
		 * Currently in parameter count mode:
		 * Iterate over all parameters for the node to which the request was
		 * originally sent, in order to find the maximum parameter ID. If a
		 * request fails, set the node's parameter count to zero.
		 */
		uint8_t node_id = result.getCallID().server_node_id.get();

		if (result.isSuccessful()) {
			warnx("UAVCAN command bridge: successful GetSet response during param count");

			uavcan::protocol::param::GetSet::Response resp = result.getResponse();
			if (resp.name.size()) {
				_param_counts[node_id] = _count_index++;

				uavcan::protocol::param::GetSet::Request req;
				req.index = _count_index;

				int call_res = _param_getset_client.call(result.getCallID().server_node_id, req);
				if (call_res < 0) {
					_count_in_progress = false;
					_count_index = 0;
					warnx("UAVCAN command bridge: couldn't send GetSet during param count: %d", call_res);
				} else {
					warnx("UAVCAN command bridge: sent GetSet during param count");
				}
			} else {
				_count_in_progress = false;
				_count_index = 0;
				warnx("UAVCAN command bridge: completed param count for node %hhu: %hhu", node_id, _param_counts[node_id]);
			}
		} else {
			_param_counts[node_id] = 0;
			_count_in_progress = false;
			_count_index = 0;
			warnx("UAVCAN command bridge: GetSet error during param count");
		}
	} else {
		/*
		 * Currently in parameter get/set mode:
		 * Publish a uORB uavcan_parameter_value message containing the current value
		 * of the parameter.
		 */
		if (result.isSuccessful()) {
			uavcan::protocol::param::GetSet::Response param = result.getResponse();

			struct uavcan_parameter_value_s response;
			response.node_id = result.getCallID().server_node_id.get();
			strncpy(response.param_id, param.name.c_str(), sizeof(response.param_id) - 1);
			response.param_id[16] = '\0';
			response.param_index = _param_index;
			response.param_count = _param_counts[response.node_id];

			if (param.value.is(uavcan::protocol::param::Value::Tag::integer_value)) {
				response.param_type = MAV_PARAM_TYPE_INT64;
				response.int_value = param.value.to<uavcan::protocol::param::Value::Tag::integer_value>();
			} else if (param.value.is(uavcan::protocol::param::Value::Tag::real_value)) {
				response.param_type = MAV_PARAM_TYPE_REAL32;
				response.real_value = param.value.to<uavcan::protocol::param::Value::Tag::real_value>();
			} else if (param.value.is(uavcan::protocol::param::Value::Tag::boolean_value)) {
				response.param_type = MAV_PARAM_TYPE_UINT8;
				response.int_value = param.value.to<uavcan::protocol::param::Value::Tag::boolean_value>();
			}

			warnx("UAVCAN command bridge: successful GetSet response for param %s, node %hhu", response.param_id, response.node_id);

			if (_param_response_pub == nullptr) {
				_param_response_pub = orb_advertise(ORB_ID(uavcan_parameter_value), &response);
			} else {
				orb_publish(ORB_ID(uavcan_parameter_value), _param_response_pub, &response);
			}
		} else {
			warnx("UAVCAN command bridge: GetSet error");
		}

		_param_in_progress = false;
		_param_index++;
	}
}

void UavcanServers::param_count(uavcan::NodeID node_id)
{
	uavcan::protocol::param::GetSet::Request req;
	req.index = 0;
	int call_res = _param_getset_client.call(node_id, req);
	if (call_res < 0) {
		warnx("UAVCAN command bridge: couldn't start parameter count: %d", call_res);
	} else {
		_count_in_progress = true;
		_count_index = 0;
		warnx("UAVCAN command bridge: starting param count");
	}
}

uavcan::NodeID UavcanServers::get_next_active_node_id(const uavcan::NodeID &base)
{
	for (int i = base.get(); i < 128; i++) {
		if (_node_info_retriever.isNodeKnown(i) && _subnode.getNodeID() != i) {
			return uavcan::NodeID(i);
		}
	}
	return base;
}

void UavcanServers::cb_enumeration_begin(const uavcan::ServiceCallResult<uavcan::protocol::enumeration::Begin> &result)
{
	uavcan::NodeID next_id = get_next_active_node_id(result.getCallID().server_node_id);

	if (!result.isSuccessful()) {
		warnx("UAVCAN ESC enumeration: begin request for node %hhu timed out.", result.getCallID().server_node_id.get());
	} else if (result.getResponse().error) {
		warnx("UAVCAN ESC enumeration: begin request for node %hhu rejected: %hhu", result.getCallID().server_node_id.get(), result.getResponse().error);
	} else {
		_esc_count++;
		warnx("UAVCAN ESC enumeration: begin request for node %hhu completed OK.", result.getCallID().server_node_id.get());
	}

	if (next_id != result.getCallID().server_node_id) {
		// Still other active nodes to send the request to
		uavcan::protocol::enumeration::Begin::Request req;
		req.parameter_name = "esc_index";
		req.timeout_sec = _esc_enumeration_active ? 65535 : 0;

		int call_res = _enumeration_client.call(next_id, req);
		if (call_res < 0) {
			warnx("UAVCAN ESC enumeration: couldn't send Begin request: %d", call_res);
		} else {
			warnx("UAVCAN ESC enumeration: sent Begin request");
		}
	} else {
		warnx("UAVCAN ESC enumeration: completed enumeration on all nodes.");
	}
}

void UavcanServers::cb_enumeration_indication(const uavcan::ReceivedDataStructure<uavcan::protocol::enumeration::Indication> &msg)
{
	// Called whenever an ESC thinks it has received user input.
	warnx("UAVCAN ESC enumeration: got indication");

	if (!_esc_enumeration_active) {
		// Ignore any messages received when we're not expecting them
		return;
	}

	// First, check if we've already seen an indication from this ESC. If so,
	// just re-issue the previous get/set request.
	int i;
	for (i = 0; i < _esc_enumeration_index; i++) {
		if (_esc_enumeration_ids[i] == msg.getSrcNodeID().get()) {
			warnx("UAVCAN ESC enumeration: already enumerated ESC ID %hhu as index %d", _esc_enumeration_ids[i], i);
			break;
		}
	}

	uavcan::protocol::param::GetSet::Request req;
	req.name = "esc_index";
	req.value.to<uavcan::protocol::param::Value::Tag::integer_value>() = i;

	int call_res = _enumeration_getset_client.call(msg.getSrcNodeID(), req);
	if (call_res < 0) {
		warnx("UAVCAN ESC enumeration: couldn't send GetSet: %d", call_res);
	} else {
		warnx("UAVCAN ESC enumeration: sent GetSet to node %hhu (index %d)", _esc_enumeration_ids[i], i);
	}
}

void UavcanServers::cb_enumeration_getset(const uavcan::ServiceCallResult<uavcan::protocol::param::GetSet> &result)
{
	if (!result.isSuccessful()) {
		warnx("UAVCAN ESC enumeration: save request for node %hhu timed out.", result.getCallID().server_node_id.get());
	} else {
		warnx("UAVCAN ESC enumeration: save request for node %hhu completed OK.", result.getCallID().server_node_id.get());

		uavcan::protocol::param::GetSet::Response resp = result.getResponse();
		uint8_t esc_index = (uint8_t)resp.value.to<uavcan::protocol::param::Value::Tag::integer_value>();
		esc_index = std::min((uint8_t)(uavcan::equipment::esc::RawCommand::FieldTypes::cmd::MaxSize - 1), esc_index);
		_esc_enumeration_index = std::max(_esc_enumeration_index, esc_index);

		_esc_enumeration_ids[esc_index] = result.getCallID().server_node_id.get();

		uavcan::protocol::param::ExecuteOpcode::Request opcode_req;
		opcode_req.opcode = opcode_req.OPCODE_SAVE;
		int call_res = _enumeration_save_client.call(result.getCallID().server_node_id, opcode_req);
		if (call_res < 0) {
			warnx("UAVCAN ESC enumeration: couldn't send ExecuteOpcode: %d", call_res);
		} else {
			warnx("UAVCAN ESC enumeration: sent ExecuteOpcode to node %hhu (index %hhu)", _esc_enumeration_ids[esc_index], esc_index);
		}
	}
}

void UavcanServers::cb_enumeration_save(const uavcan::ServiceCallResult<uavcan::protocol::param::ExecuteOpcode> &result)
{
	uavcan::equipment::indication::BeepCommand beep;

	if (!result.isSuccessful()) {
		warnx("UAVCAN ESC enumeration: save request for node %hhu timed out.", result.getCallID().server_node_id.get());
		beep.frequency = 880.0f;
		beep.duration = 1.0f;
	} else if (!result.getResponse().ok) {
		warnx("UAVCAN ESC enumeration: save request for node %hhu rejected", result.getCallID().server_node_id.get());
		beep.frequency = 880.0f;
		beep.duration = 1.0f;
	} else {
		warnx("UAVCAN ESC enumeration: save request for node %hhu completed OK.", result.getCallID().server_node_id.get());
		beep.frequency = 440.0f;
		beep.duration = 0.25f;
	}

	(void)_beep_pub.broadcast(beep);

	if (_esc_enumeration_index == uavcan::equipment::esc::RawCommand::FieldTypes::cmd::MaxSize - 1 ||
			_esc_enumeration_index == _esc_count - 1) {
		_esc_enumeration_active = false;

		// Tell all ESCs to stop enumerating
		uavcan::protocol::enumeration::Begin::Request req;
		req.parameter_name = "esc_index";
		req.timeout_sec = 0;
		int call_res = _enumeration_client.call(get_next_active_node_id(1), req);
		if (call_res < 0) {
			warnx("UAVCAN ESC enumeration: couldn't send Begin request to stop enumeration: %d", call_res);
		} else {
			warnx("UAVCAN ESC enumeration: sent Begin request to stop enumeration");
		}
	}
}
