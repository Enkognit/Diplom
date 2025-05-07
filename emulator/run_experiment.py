from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from functools import partial
import subprocess
import sys
import os
import asyncio
import time
import logging
import argparse
import yaml
from typing import Union, List, Dict, Any, Tuple

import docker
import docker.errors
import docker.types

from pathlib import Path

from ipaddress import ip_network

BASE_IMAGE = "ubuntu:24.04"
WAIT_SECONDS = 10
SUBNET_POOL_BASE = "10.0.0.0/8"
SUBNET_PREFIX = 24


logging.basicConfig(
    level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s"
)

class SubnetManager:
    def __init__(self, base_pool_cidr, prefix):
        self.base_pool = ip_network(base_pool_cidr)
        self.prefix = prefix
        if not self.base_pool.prefixlen < prefix:
            raise ValueError(
                f"Base pool prefix ({self.base_pool.prefixlen}) must be smaller than subnet prefix ({prefix})"
            )
        self._subnet_generator = self.base_pool.subnets(new_prefix=self.prefix)
        self.allocated_subnets = set()

    def get_next_subnet(self) -> str:
        while True:
            try:
                subnet = next(self._subnet_generator)
                # Basic check, real collision check could be more robust
                if str(subnet) not in self.allocated_subnets:
                    self.allocated_subnets.add(str(subnet))
                    logging.debug(f"Allocated subnet: {subnet}")
                    return str(subnet)
            except StopIteration:
                raise RuntimeError("Ran out of subnets in the base pool!")


def count_next_result_id(logs_path: Path) -> int:
    if not logs_path.exists():
        os.makedirs(logs_path)

    cnt = 0
    for file_name in os.listdir(logs_path):
        if (logs_path / file_name).is_dir() and str(file_name).isnumeric():
            cnt = max(int(file_name), cnt)

    return cnt + 1

Edge = Tuple[int, int]

@dataclass
class NetworkValue:
    mean: int
    jitter: int = 0

@dataclass
class EdgeProperties:
    conn_lat: NetworkValue
    lat: NetworkValue
    bw: int

@dataclass
class ExperimentSetup:
    duration: int
    conn_latency: NetworkValue
    latency: NetworkValue
    bandwidth: int

@dataclass
class ExperimentMods:
    time: int
    add: Dict[Edge, EdgeProperties]
    erase: List[Edge]

@dataclass
class ExperimentInit:
    nodes_amount: int
    edges: Dict[Edge, EdgeProperties]

@dataclass
class ExperimentConfig:
    setup: ExperimentSetup
    init: ExperimentInit
    mods: List[ExperimentMods]

DURATION = 5
NODES_AMOUNT = 5
CONN_LATENCY = NetworkValue(1300, 300)
LATENCY = NetworkValue(100, 50)
BANDWIDTH = 50000000

class ExperimentConfigParser:

    def parse_network_value(self, value: Union[int, List[int], None], default: NetworkValue) -> NetworkValue:
        if value is None:
            return default
        if isinstance(value, int):
            return NetworkValue(mean=value)
        if isinstance(value, list):
            if len(value) == 1:
                return NetworkValue(mean=value[0])
            if len(value) == 2:
                return NetworkValue(mean=value[0], jitter=value[1])
        raise ValueError(f"Invalid network value format: {value}")

    def parse_edge(self, edge) -> Edge:
         err = ValueError(f"Invalid edge format: {edge}")
         if isinstance(edge, str):
            try:
                redge = tuple(map(int, edge.split(' ')));
            except Exception as e:
                raise err from e
            if len(redge) != 2 or not (isinstance(redge[0], int) and isinstance(redge[1], int)):
                raise err
            return redge
         else:   
            raise err

    def parse_edges(self, edges_data: Dict, setup: ExperimentSetup, nodes_amount: int) -> Dict[Edge, EdgeProperties]:
        if not edges_data:
            return {}
        edges: Dict[Edge, EdgeProperties] = {}
        for edge, params in edges_data.items():
            edge = self.parse_edge(edge)
            if min(edge) < 1 or max(edge) > nodes_amount:
                raise ValueError(f"Edge {edge[0]} - {edge[1]} is out of bounds")
            
            edges[edge] = EdgeProperties(
                conn_lat=self.parse_network_value(
                    params.get('conn_lat'), setup.conn_latency),
                lat=self.parse_network_value(
                    params.get('lat'), setup.latency),
                bw=params.get('bw', setup.bandwidth)
            )
        return edges

    def parse_mods(self, mods_data: List[Dict], init: ExperimentInit, setup: ExperimentSetup) -> List[ExperimentMods]:
        mods: List[ExperimentMods] = []
        for mod in mods_data:
            mods.append(ExperimentMods(
                time=int(mod['time']),
                add=self.parse_edges(mod.get('add', {}), setup, init.nodes_amount),
                erase=[self.parse_edge(e) for e in mod.get('erase', [])]
            ))
        return mods

    def parse_config(self, file_path: str) -> ExperimentConfig:
        with open(file_path, 'r') as file:
            config_data = yaml.safe_load(file) or {}

        # Parsing setup section
        setup_data = config_data.get('setup', {})
        setup = ExperimentSetup(
            duration=setup_data.get('duration', 5),
            conn_latency=self.parse_network_value(
                setup_data.get('conn_latency'), NetworkValue(1300, 300)),
            latency=self.parse_network_value(
                setup_data.get('latency'), NetworkValue(100, 20)),
            bandwidth=setup_data.get('bandwidth', 5)
        )

        # Parsing init section
        init_data = config_data.get('init', {})
        nodes_amount = init_data.get('nodes_amount', 5)
        init = ExperimentInit(
            nodes_amount=nodes_amount,
            edges=self.parse_edges(init_data.get('edges', {}), setup, nodes_amount)
        )

        # Parsing mods section
        mods = self.parse_mods(config_data.get('mods', []), init, setup)

        return ExperimentConfig(setup=setup, init=init, mods=mods)

class TrafficShaper:
    def __init__(self, interface):
        print('Create traffic shaper')
        self.interface = interface

    def shell(self, cmd):
        try:
            subprocess.run(cmd, shell=True, check=True, capture_output=True)
            return True
        except subprocess.CalledProcessError as e:
            print(f"Error: {e.stderr.decode()}")
            return False

    def set_rules(self, syn_delay_ms, psh_delay_ms, bandwidth_kbps):
        self.clear_rules()
        
        print(self.interface, syn_delay_ms, psh_delay_ms, bandwidth_kbps)

        # # 1. Создаем корневую HTB-очередь для контроля bandwidth
        # if not self.shell(f"tc qdisc add dev {self.interface} root handle 1: htb default 3"):
        #     raise RuntimeError("")

        # # 2. Ограничиваем пропускную способность (если задана)
        # if bandwidth_kbps:
        #     if not self.shell(f"tc class add dev {self.interface} parent 1: classid 1:1 htb rate {bandwidth_kbps}kbit"):
        #         self.clear_rules()
        #         raise RuntimeError("")

        # # 3. Создаем под-очередь prio для разделения трафика
        # if not self.shell(f"tc qdisc add dev {self.interface} parent 1:1 handle 2: prio bands 3"):
        #     self.clear_rules()
        #     raise RuntimeError("Не удалось создать prio-очередь")

        # # 4. Настраиваем задержку для SYN (класс 2:1)
        # if syn_delay_ms:
        #     if not self.shell(f"tc qdisc add dev {self.interface} parent 2:1 handle 21: netem delay {syn_delay_ms}ms"):
        #         self.clear_rules()
        #         raise RuntimeError("Не удалось добавить задержку SYN")

        #     # Фильтр для SYN (0x02)
        #     if not self.shell(
        #         f"tc filter add dev {self.interface} parent 2: protocol ip "
        #         f"u32 match ip protocol 6 0xff match u8 0x02 0xff at nexthdr+13 flowid 2:1"
        #     ):
        #         self.clear_rules()
        #         raise RuntimeError("Не удалось добавить фильтр SYN")

        # # 5. Настраиваем задержку для PSH (класс 2:2)
        # if psh_delay_ms:
        #     if not self.shell(f"tc qdisc add dev {self.interface} parent 2:2 handle 22: netem delay {psh_delay_ms}ms"):
        #         self.clear_rules()
        #         raise RuntimeError("Не удалось добавить задержку PSH")

        #     # Фильтр для PSH (0x08), но исключаем ACK (0x10)
        #     if not self.shell(
        #         f"tc filter add dev {self.interface} parent 2: protocol ip "
        #         f"u32 match ip protocol 6 0xff "
        #         f"match u8 0x08 0xff at nexthdr+13 "
        #         # f"match u8 0x10 0x00 at nexthdr+13 "  # Исключаем ACK (0x10)
        #         f"flowid 2:2"
        #     ):
        #         self.clear_rules()
        #         raise RuntimeError("Не удалось добавить фильтр PSH")

        print("Правила успешно применены!")

    def clear_rules(self):
        print("clear_rules")
        # self.shell(f"tc qdisc del dev {self.interface} root 2>/dev/null || true")

    def __del__(self):
        print("del")
        self.clear_rules()

class ExperimentRunner:
    
    bridges: Dict[str, Any]
    shapers: Dict[str, TrafficShaper]
    
    def __init__(self, binary: Path, logs: Path):
        self.binary = binary
        self.logs = logs
        self.subnet_manager = SubnetManager(SUBNET_POOL_BASE, SUBNET_PREFIX)
        self.containers = {}
        self.bridges = {}
        self.shapers = {}

        try:
            self.docker_client = docker.from_env()
            self.docker_client.ping()
            logging.info("Connected to Docker daemon.")
        except Exception as e:
            logging.error(f"Failed to connect to Docker daemon: {e}")
            logging.error("Please ensure Docker is running and accessible.")
            sys.exit(1)
    
    def add_bridge(self, edge: Edge, props: EdgeProperties):
        u, v = edge
        if u > v:
            u, v = v, u
        network_name = f"net_{u}_{v}"
        if network_name not in self.bridges.keys():
            print('Trying to add', network_name)
            try:
                container_u = self.containers.get(u)
                container_v = self.containers.get(v)
                
                print('Got containers')

                if not container_u or not container_v:
                    logging.warning(
                        f"Container missing for connection {u}-{v}, skipping connect."
                    )
                    return
                subnet = self.subnet_manager.get_next_subnet()
                ipam_pool = docker.types.IPAMPool(subnet=subnet)
                ipam_config = docker.types.IPAMConfig(pool_configs=[ipam_pool])
                
                network = self.docker_client.networks.create(
                    network_name,
                    driver="bridge",
                    ipam=ipam_config,
                    internal=True,
                )
                self.bridges[network_name] = network
                print('++++++')
                
                logging.info(
                    f"Created network: {network.name} with subnet {subnet}"
                )

                network.connect(container_u)
                network.connect(container_v)
                
                logging.info(
                    f"Connected container {container_u.name} to {network.name}"
                )
                logging.info(
                    f"Connected container {container_v.name} to {network.name}"
                )

            except Exception as e:
                logging.error(
                    f"Unexpected error setting up network {network_name}: {e}"
                )
                self.cleanup()
                sys.exit(1)
        
        print('WTF1')
        interface = f"br-{self.docker_client.networks.get(network_name).attrs['Id'][:12]}"
        self.shapers.update({network_name: TrafficShaper(interface=interface)})
        print('WTF2')
        try:
            self.shapers[network_name].set_rules(props.conn_lat, props.lat, props.bw)
        except Exception as e:
            print("Error shaping:", e)
        print('WTF3')
        
    def remove_bridge(self, edge: Edge):
        u, v = edge
        if u > v:
            u, v = v, u
        network_name = f"net_{u}_{v}"
        
        print(self.bridges.keys())
        print(network_name in self.bridges.keys())
        
        if network_name in self.bridges.keys():
            print('Yes')
            try:
                print('Remove net', network_name)
                self.shapers.pop(network_name)
                self.bridges[network_name].remove()
                self.bridges.pop(network_name)
            except Exception as e:
                print(f"Error: ", e.with_traceback())
        
        print('Erased')
            

    def create_devices(self, num_containers):
        logging.info(f"Creating {num_containers} devices...")

        if not os.path.exists(self.binary):
            logging.error(f"Binary not found at: {self.binary}")
            sys.exit(1)

        try:
            self.docker_client.images.get(BASE_IMAGE)
        except docker.errors.ImageNotFound:
            logging.info(f"Pulling base image: {BASE_IMAGE}...")
            self.docker_client.images.pull(BASE_IMAGE)
        except Exception as e:
            logging.error(f"Error checking/pulling base image: {e}")
            sys.exit(1)

        for i in range(1, num_containers + 1):
            container_name = f"dist_node_{i}"

            try:
                container = self.docker_client.containers.run(
                    BASE_IMAGE,
                    name=container_name,
                    hostname=f"node{i}",
                    detach=True,
                    network=None,
                    network_mode="bridge",
                    tty=True,
                    stdin_open=True,
                    volumes={
                        self.binary: {
                            "bind": f"/work/{self.binary.name}",
                            "mode": "ro",
                        },
                        self.logs / "logs": {"bind": "/work/logs", "mode": "rw"},
                        self.logs
                        / "visualization": {
                            "bind": "/work/visualization",
                            "mode": "rw",
                        },
                        self.logs
                        / "statistics": {"bind": "/work/statistics", "mode": "rw"},
                        # Optional: Mount a host directory to collect coredumps
                        # Adjust host path as needed, ensure it exists
                        # os.path.abspath(f'./coredumps_{i}'): {'bind': '/coredumps', 'mode': 'rw'}
                    },  # type: ignore
                    # Set ulimit for coredumps (requires host support and privileges)
                    # ulimits=[docker.types.Ulimit(name='core', soft=-1, hard=-1)],
                    # command="sleep infinity" # Keep container alive initially
                    command=[
                        "/bin/sh",
                        "-c",
                        f"chmod +x /work/{self.binary.name}; sleep infinity",
                    ],
                )

                self.containers[i] = container

                logging.info(
                    f"Created container: {container.name} (ID: {container.short_id})"
                )
            except Exception as e:
                logging.error(
                    f"Error creating container {container_name}: {e}"
                )
                self.cleanup()
                sys.exit(1)
                
        # --- Step 2: Disconnect all containers from the default bridge network ---
        logging.info("Disconnecting containers from the default bridge network...")
        # Find the default bridge network (common names: 'bridge', 'docker0')
        # Using 'bridge' is standard for Docker Desktop / newer Docker Engine
        print(self.docker_client.networks.list())
        default_bridge_name = "bridge"
        try:
            bridge_network = self.docker_client.networks.get(default_bridge_name)

            for container in self.containers.values():
                try:
                    logging.info(
                        f"Disconnecting {container.name} from network '{default_bridge_name}'..."
                    )
                    bridge_network.disconnect(container)
                    logging.info(
                        f"Successfully disconnected {container.name} from '{default_bridge_name}'."
                    )
                except docker.errors.APIError as e:
                    # Handle cases where it might already be disconnected or other API issues
                    # Example: 404 Client Error ... ("Container xxx not found in network bridge") - This is OK if it happens.
                    if (
                        e.response is not None
                        and e.response.status_code == 404
                        and "not found in network" in str(e)
                    ):
                        logging.warning(
                            f"Container {container.name} was already not connected to '{default_bridge_name}'."
                        )
                    else:
                        # Log other errors but continue trying to disconnect others
                        logging.error(
                            f"Error disconnecting {container.name} from '{default_bridge_name}': {e}"
                        )
                except Exception as e:
                    logging.error(
                        f"Unexpected error disconnecting {container.name} from '{default_bridge_name}': {e}"
                    )

        except docker.errors.NotFound:
            logging.error(
                f"Default bridge network '{default_bridge_name}' not found. Skipping disconnection step."
            )
        except docker.errors.APIError as e:
            logging.error(
                f"API error getting default bridge network '{default_bridge_name}': {e}. Skipping disconnection step."
            )
        except Exception as e:
            logging.error(
                f"Unexpected error getting/using default bridge network: {e}. Skipping disconnection step."
            )

    def run_experiment(self, config: ExperimentConfig):
        self.create_devices(config.init.nodes_amount)
        
        def run_binary_in_device(i: int):
            container = self.containers.get(i)
            if not container:
                logging.warning(f"Container {i} not found, skipping binary start.")
                return

            try:
                container.exec_run(cmd=f"touch /work/logs/log_{i}")
                exec_cmd = f'bash -c "/work/{self.binary.name} {i} {WAIT_SECONDS} > /work/logs/log_{i} 2>&1"'
                exit_code, _ = container.exec_run(
                    cmd=exec_cmd, user="root", detach=True
                )

                logging.info(
                    f"Started '{self.binary.name}' in container {container.name}."
                )
                if exit_code is not None:
                    logging.error(f"Started with error: {exit_code}")
            except docker.errors.APIError as e:
                logging.error(
                    f"Error starting binary in container {container.name}: {e}"
                )
            except Exception as e:
                logging.error(
                    f"Unexpected error starting binary in container {container.name}: {e}"
                )
                return
        
        with ThreadPoolExecutor(max_workers=10) as executor:
            executor.map(run_binary_in_device, list(range(1, config.init.nodes_amount + 1)))    
        
        # Init
        for edge, props in config.init.edges.items():
            self.add_bridge(edge, props)
            
        async def delayed_task(task, delay):
            await asyncio.sleep(delay / 1000)
            task()
            
        self.time = time.time()
            
        async def run_exp(make_mod):
            tasks: List[asyncio.Task] = []
            for mod in config.mods:
                tasks.append(asyncio.create_task(delayed_task(partial(make_mod, mod), mod.time)))
            
            await asyncio.sleep(config.setup.duration)
            
            for task in tasks:
                task.cancel()
        
        asyncio.run(run_exp(self.make_modification))
            
    def make_modification(self, mods: ExperimentMods):
        print('Make mod:', time.time() - self.time)
        print(mods)
        for edge, props in mods.add.items():
            print('Add:',edge)
            self.add_bridge(edge, props)
            print('Added:',edge)
        for edge in mods.erase:
            print('Erase:',edge)
            try:
                self.remove_bridge(edge)
            except e:
                print('WTF: ', e)
            print('Erased:',edge)
        print('End mod:', time.time() - self.time)
            

    def check_container_status(self):
        """Checks the status of containers after the experiment."""
        logging.info("Checking container status...")
        crashed = False
        for container in self.containers.values():
            try:
                container.reload()  # Get fresh status
                status = container.status
                logging.info(f"Container {container.name}: Status = {status}")
                if status != "running":
                    logging.warning(
                        f"Container {container.name} is not running (status: {status}). It might have crashed."
                    )
                    crashed = True
                    # You could try inspecting the exec result here if needed, but it's complex
                    # exec_id = exec_results.get(i, {}).get('id')
                    # if exec_id:
                    #     exec_info = self.docker_client.api.exec_inspect(exec_id)
                    #     logging.info(f"  Exec exit code: {exec_info.get('ExitCode')}")

            except docker.errors.NotFound:
                logging.warning(
                    f"Container {container.name} not found during status check."
                )
                crashed = True
            except docker.errors.APIError as e:
                logging.error(
                    f"Error checking status for container {container.name}: {e}"
                )
            except Exception as e:
                logging.error(
                    f"Unexpected error checking status for container {container.name}: {e}"
                )

        if crashed:
            logging.warning("One or more containers were not in a 'running' state.")
            logging.warning(
                "Check logs in './container_logs/' and potential coredumps."
            )
        else:
            logging.info("All containers appear to be running.")

    def cleanup(self):
        """Stops and removes containers and networks."""
        logging.info("Cleaning up resources...")
        
        async def remove_container(container):
            try:
                logging.info(f"Stopping container {container.name}...")
                container.stop(timeout=1)  # Give 5 seconds to stop gracefully
            except docker.errors.NotFound:
                logging.warning(f"Container {container.name} already removed.")
            except docker.errors.APIError as e:
                logging.warning(
                    f"Error stopping container {container.name}: {e}. Force removing."
                )
            except Exception as e:
                logging.warning(
                    f"Unexpected error stopping container {container.name}: {e}. Force removing."
                )
            finally:
                # Ensure removal even if stop fails
                try:
                    logging.info(f"Removing container {container.name}...")
                    # Reload might be needed if state is inconsistent after error
                    try:
                        container.reload()
                    except Exception:
                        pass
                    if container.status != "removed":
                        container.remove(
                            force=True
                        )  # Force remove if stop failed or stuck
                except Exception as e:
                    logging.error(
                        f"Unexpected error removing container {container.name}: {e}"
                    )

        async def remove_all_containers():
            tasks = [
                asyncio.create_task(remove_container(container))
                for container in self.containers.values()
            ]
            for task in tasks:
                await task
            
        asyncio.run(remove_all_containers())

        # Remove networks
        for network in self.bridges.values():
            try:
                logging.info(f"Removing network {network.name}...")
                network.remove()
            except docker.errors.NotFound:
                logging.warning(f"Network {network.name} already removed.")
            except docker.errors.APIError as e:
                logging.error(f"Error removing network {network.name}: {e}")
            except Exception as e:
                logging.error(f"Unexpected error removing network {network.name}: {e}")

        logging.info("Cleanup complete.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Deploy a distributed system in Docker containers based on a config file."
    )
    parser.add_argument("-b", "--binary-path", help="Path to algorithm binary.", required=True)
    parser.add_argument(
        "-c", "--config-file", help="Path to the experiment configuration file.", required=True
    )
    parser.add_argument("-l", "--logs-path", help="Path to directory for retreiving logs.", required=True)
    
    args = parser.parse_args()        

    binary_path = Path(args.binary_path).absolute()
    config_path = Path(args.config_file).absolute()
    logs_path = Path(args.logs_path).absolute()
    
    if not binary_path.is_file():
        logging.error(f"Cannot find binary at specified path: {binary_path}")
        sys.exit(1)
        
    if not config_path.is_file():
        logging.error(f"Cannot find config at specified path: {config_path}")
        sys.exit(1)

    logs_num_path = logs_path / str(count_next_result_id(logs_path))

    experiment_config = ExperimentConfigParser().parse_config(config_path)

    runner = ExperimentRunner(binary_path, logs_num_path)

    try:
        runner.run_experiment(experiment_config)
    except KeyboardInterrupt:
        logging.warning("Keyboard interrupt received. Initiating cleanup...")
    except Exception as e:
        logging.error(f"An error occurred during execution: {e}")
        runner.cleanup()
        sys.exit(1)
    finally:
        runner.cleanup()

        # scripts paths
        check_finish_path = os.path.abspath(
            os.path.dirname(os.path.abspath(__file__)) + "/check_finish.py"
        )
        combine_logs_script_path = os.path.abspath(
            os.path.dirname(os.path.abspath(__file__))
            + "/combine_logs.py"
        )

        # transfer rights to current user
        p = subprocess.run(
            f"sudo chown -R $(id -u):$(id -g) {logs_num_path}; sudo chmod -R u+r {logs_num_path}; sudo chmod u+rx {check_finish_path}",  # u+r for visualizer access
            shell=True,
            capture_output=True,
        )
        # check metrics
        p = subprocess.run(
            f"python3 {check_finish_path} -r {logs_num_path} -t {WAIT_SECONDS - 1}",
            shell=True,
            capture_output=True,
        )
        exit_code = p.returncode
        print(p.stdout.decode())
        sys.stderr.write(p.stderr.decode())

        # create logs for visualizer
        p = subprocess.run(
            f"python3 {combine_logs_script_path} -p {logs_num_path}/visualization -o {logs_num_path}/visres -g",
            shell=True,
            capture_output=True,
        )
        print(p.stdout.decode())
        sys.stderr.write(p.stderr.decode())
        sys.exit(exit_code)
