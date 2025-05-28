from concurrent.futures import ThreadPoolExecutor
import copy
from dataclasses import dataclass
from datetime import datetime
from functools import partial
import ipaddress
import combine_logs
import subprocess
import sys
import os
import asyncio
import logging
import argparse
import yaml

from typing import Union, List, Dict, Any, Tuple

import docker
import docker.errors
import docker.types

from pathlib import Path

from ipaddress import ip_network

BASE_IMAGE = "emulator_image"
SUBNET_POOL_BASE = "10.0.0.0/8"
SUBNET_PREFIX = 24
MAX_WORKERS = 100

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s - %(levelname)s - %(message)s")


def shell(command):
    return subprocess.run(command,
                          shell=True,
                          check=True,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


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
    add: List[Edge]
    erase: List[Edge]


@dataclass
class ExperimentInit:
    nodes_amount: int
    custom: Dict[Edge, EdgeProperties]
    edges: List[Edge]


@dataclass
class ExperimentConfig:
    setup: ExperimentSetup
    init: ExperimentInit
    mods: List[ExperimentMods]


DURATION = 5
NODES_AMOUNT = 5
CONN_LATENCY = NetworkValue(800, 200)
LATENCY = NetworkValue(20, 0)
BANDWIDTH = 1000000


class ExperimentConfigParser:

    def parse_network_value(self, value: Union[int, List[int], None],
                            default: NetworkValue) -> NetworkValue:
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
                redge = tuple(map(int, edge.split(' ')))
            except Exception as e:
                raise err from e
            if len(redge) != 2 or not (isinstance(redge[0], int)
                                       and isinstance(redge[1], int)):
                raise err
            return (min(redge), max(redge))
        else:
            raise err
        
    def parse_visib(self, edges_data: Dict, nodes_amount: int) -> List[Edge]:
        edges = []
        for edge, params in edges_data.items():
            if isinstance(edge, int):
                if edge < 1 or edge > nodes_amount:
                    raise ValueError(
                        f"Node {edge} is out of bounds")
                for i in params:
                    if not isinstance(i, int):
                        raise ValueError(f"Element {i} must be int")
                    if i < 1 or i > nodes_amount:
                        raise ValueError(
                            f"Node {i} is out of bounds")
                    redge = (edge, i)
                    redge = (min(redge), max(redge))
                    edges.append(redge)
        return edges

    def parse_edges(self, edges_data: Dict, setup: ExperimentSetup,
                    nodes_amount: int) -> Dict[Edge, EdgeProperties]:
        if not edges_data:
            return {}
        edges: Dict[Edge, EdgeProperties] = {}
        for edge, params in edges_data.items():
            edge = self.parse_edge(edge)
            if min(edge) < 1 or max(edge) > nodes_amount:
                raise ValueError(
                    f"Edge {edge[0]} - {edge[1]} is out of bounds")

            edges[edge] = EdgeProperties(
                conn_lat=self.parse_network_value(params.get('conn_lat'),
                                                setup.conn_latency),
                lat=self.parse_network_value(params.get('lat'), setup.latency),
                bw=params.get('bw', setup.bandwidth))
        return edges

    def parse_mods(self, mods_data: List[Dict]) -> List[ExperimentMods]:
        mods: List[ExperimentMods] = []
        for mod in mods_data:
            mods.append(
                ExperimentMods(
                    time=int(mod['time']),
                    add=[self.parse_edge(e) for e in mod.get('add', [])],
                    erase=[self.parse_edge(e) for e in mod.get('erase', [])]))
        return mods

    def parse_config(self, file_path: str) -> ExperimentConfig:
        with open(file_path, 'r') as file:
            config_data = yaml.safe_load(file) or {}

        setup_data = config_data.get('setup', {})
        setup = ExperimentSetup(duration=setup_data.get('duration', DURATION),
                                conn_latency=self.parse_network_value(
                                    setup_data.get('conn_latency'),
                                    CONN_LATENCY),
                                latency=self.parse_network_value(
                                    setup_data.get('latency'),
                                    LATENCY),
                                bandwidth=setup_data.get('bandwidth', BANDWIDTH))

        init_data = config_data.get('init', {})
        nodes_amount = init_data.get('nodes_amount', NODES_AMOUNT)
        init = ExperimentInit(nodes_amount=nodes_amount,
                              custom=self.parse_edges(init_data.get('custom_shapes', {}), setup, nodes_amount),
                              edges=self.parse_visib(init_data.get('edges', []), nodes_amount))
        mods = self.parse_mods(config_data.get('mods', []))

        return ExperimentConfig(setup=setup, init=init, mods=mods)

class TrafficShaper:

    def __init__(self, interface):
        self.interface = interface

    def set_rules(self, conn_delay: NetworkValue, trans_delay: NetworkValue,
                  bandwidth_kbps: int):
        # logging.info(f"Shaping {self.interface}")
        
        syn_delay_ms = copy.copy(conn_delay)
        urg_delay_ms = copy.copy(trans_delay)
        
        syn_delay_ms.mean -= urg_delay_ms.mean
        syn_delay_ms.jitter = abs(syn_delay_ms.jitter - urg_delay_ms.jitter)
        
        if syn_delay_ms.mean < 0:
            raise Exception("conn_lat must be > then lat")

        try:
#             command = f"""sudo tc qdisc add dev {self.interface} root handle 1: prio
# sudo tc qdisc add dev {self.interface} parent 1:1 handle 10: netem delay {syn_delay_ms.mean}ms {syn_delay_ms.jitter}ms
# sudo tc qdisc add dev {self.interface} parent 1:2 handle 20: netem delay {urg_delay_ms.mean}ms {urg_delay_ms.jitter}ms rate {bandwidth_kbps}kbit
# sudo tc qdisc add dev {self.interface} parent 1:3 handle 30: pfifo_fast
# sudo iptables -t mangle -A FORWARD -j MARK --set-mark 10
# sudo tc filter add dev {self.interface} parent 1:0 protocol ip prio 10 handle 10 fw flowid 1:3
# sudo iptables -t mangle -A FORWARD -p tcp --tcp-flags URG URG -j MARK --set-mark 2
# sudo tc filter add dev {self.interface} parent 1:0 protocol ip prio 2 handle 2 fw flowid 1:2
# sudo iptables -t mangle -A FORWARD -p tcp --tcp-flags FIN FIN -j MARK --set-mark 3
# sudo tc filter add dev {self.interface} parent 1:0 protocol ip prio 3 handle 3 fw flowid 1:2
# sudo iptables -t mangle -A FORWARD -p tcp --tcp-flags SYN,ACK SYN -j MARK --set-mark 1
# sudo tc filter add dev {self.interface} parent 1:0 protocol ip prio 1 handle 1 fw flowid 1:1"""
#             print(command)
#             shell(command)
            shell(
                f"sudo tc qdisc add dev {self.interface} root handle 1: prio")
            shell(
                f"sudo tc qdisc add dev {self.interface} parent 1:1 handle 10: netem delay {syn_delay_ms.mean}ms {syn_delay_ms.jitter}ms"
            )
            shell(
                f"sudo tc qdisc add dev {self.interface} parent 1:2 handle 20: netem delay {urg_delay_ms.mean}ms {urg_delay_ms.jitter}ms rate {bandwidth_kbps}kbit"
            )
            shell(
                f"sudo tc qdisc add dev {self.interface} parent 1:3 handle 30: pfifo_fast"
            )
            # ALL
            shell(f"sudo iptables -t mangle -A FORWARD -j MARK --set-mark 10")
            shell(
                f"sudo tc filter add dev {self.interface} parent 1:0 protocol ip prio 10 handle 10 fw flowid 1:3"
            )
            # URG
            shell(
                f"sudo iptables -t mangle -A FORWARD -p tcp --tcp-flags URG URG -j MARK --set-mark 2"
            )
            shell(
                f"sudo tc filter add dev {self.interface} parent 1:0 protocol ip prio 2 handle 2 fw flowid 1:2"
            )
            # FIN
            shell(
                f"sudo iptables -t mangle -A FORWARD -p tcp --tcp-flags FIN FIN -j MARK --set-mark 3"
            )
            shell(
                f"sudo tc filter add dev {self.interface} parent 1:0 protocol ip prio 3 handle 3 fw flowid 1:2"
            )
            # SYN
            shell(
                f"sudo iptables -t mangle -A FORWARD -p tcp --tcp-flags SYN,ACK SYN -j MARK --set-mark 1"
            )
            shell(
                f"sudo tc filter add dev {self.interface} parent 1:0 protocol ip prio 1 handle 1 fw flowid 1:1"
            )

        except subprocess.CalledProcessError as e:
            logging.error("Error shaping interface:", e.stdout, e.stderr)
            self.clear_rules()

    def clear_rules(self):
        logging.info(f"Clearing rules on inteface {self.interface}")
        try:
            shell(f"sudo tc qdisc del dev {self.interface} root")
        except subprocess.CalledProcessError as e:
            pass


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
            self.docker_client = docker.DockerClient(max_pool_size=MAX_WORKERS, timeout=10)
            self.docker_client.ping()
            logging.info("Connected to Docker daemon.")
        except Exception as e:
            logging.error(f"Failed to connect to Docker daemon: {e}")
            logging.error("Please ensure Docker is running and accessible.")
            sys.exit(1)

    def get_interface_from_container(self, container, ip):
        container.reload()
        addrs = container.exec_run(f"ip -o addr show").output.decode('utf-8').splitlines()
        for line in addrs:
            if ip in line:
                return line.split(' ')[1]

    def add_bridge(self, edge: Edge, props: EdgeProperties):
        u, v = edge
        if u > v:
            u, v = v, u
        network_name = f"net_{u}_{v}"
        
        if network_name not in self.bridges.keys():
            try:
                container_u = self.containers.get(u)
                container_v = self.containers.get(v)

                if not container_u or not container_v:
                    logging.warning(
                        f"Container missing for connection {u}-{v}, skipping connect."
                    )
                    return
                subnet = self.subnet_manager.get_next_subnet()
                ip_subnet = ipaddress.IPv4Network(subnet)

                ip_h: str = None
                ip_u: str = None
                ip_v: str = None
                for ip in ip_subnet.hosts():
                    if not ip_h:
                        ip_h = str(ip)
                    elif not ip_u:
                        ip_u = str(ip)
                    elif not ip_v:
                        ip_v = str(ip)
                    else:
                        break

                ipam_pool = docker.types.IPAMPool(subnet=subnet, gateway=ip_h)
                ipam_config = docker.types.IPAMConfig(pool_configs=[ipam_pool])

                network = self.docker_client.networks.create(network_name,
                                                             driver="bridge",
                                                             ipam=ipam_config)
                self.bridges[network_name] = network

                logging.info(
                    f"Created network: {network.name} with subnet {subnet}")

                # interface = f"br-{self.docker_client.networks.get(network_name).attrs['Id'][:12]}"
                
                network.connect(container_u, ipv4_address=ip_u)
                container_u.exec_run(f'ip link set {self.get_interface_from_container(container_u, ip_u)} down')
                
                network.connect(container_v, ipv4_address=ip_v)
                container_v.exec_run(f'ip link set {self.get_interface_from_container(container_v, ip_v)} down')

                interface = f"br-{self.docker_client.networks.get(network_name).attrs['Id'][:12]}"
                self.shapers.update({network_name: TrafficShaper(interface)})
                self.shapers[network_name].set_rules(props.conn_lat, props.lat, props.bw)

            except Exception as e:
                logging.error(
                    f"Unexpected error setting up network {network_name}: {e}")
                self.cleanup()
                sys.exit(1)

    def bridge_up(self, edge: Edge):
        u, v = edge
        if u > v:
            u, v = v, u
        network_name = f"net_{u}_{v}"
        
        logging.info(f"Up {network_name}")

        if network_name in self.bridges.keys():
            try:
                # logging.info("Start up " + network_name)
                container_u = self.containers.get(u)
                container_v = self.containers.get(v)
                ip_u = container_u.attrs['NetworkSettings']['Networks'][network_name]['IPAddress']
                ip_v = container_v.attrs['NetworkSettings']['Networks'][network_name]['IPAddress']
                ip_h = self.bridges[network_name].attrs['IPAM']['Config'][0]['Gateway']
                
                eth_u = self.get_interface_from_container(container_u, ip_u)
                eth_v = self.get_interface_from_container(container_v, ip_v)
                
                # logging.info("Mid up " + network_name)
                
                # print(container_v.exec_run(f'sh -c \'iptables -A INPUT -i {eth_v} -j DROP && iptables -A OUTPUT -o {eth_v} -j DROP && ip link set {eth_v} up && ip route add {ip_u} via {ip_h} && iptables-save | grep -v -- \'-i eth1\\|-o eth1\' | iptables-restore && ip link show\''))
                # if self.start_time:
                #     print("Shaping " + network_name, (datetime.now() - self.start_time).total_seconds())
                container_u.exec_run(f'sh -c \'ip link set {eth_u} up && ip route add {ip_v} via {ip_h}\'')
                container_v.exec_run(f'sh -c \'ip link set {eth_v} up && ip route add {ip_u} via {ip_h}\'')
                # container_u.exec_run(f'sh -c \'iptables -A INPUT -i {eth_u} -j DROP && ip link set {eth_u} up && ip route add {ip_v} via {ip_h}\'')
                # container_v.exec_run(f'sh -c \'iptables -A INPUT -i {eth_v} -j DROP && ip link set {eth_v} up && ip route add {ip_u} via {ip_h}\'')
                # container_u.exec_run(f'sh -c \'iptables -D INPUT -i {eth_u} -j DROP && ip link show\'')
                # container_v.exec_run(f'sh -c \'iptables -D INPUT -i {eth_v} -j DROP && ip link show\'')
                
                # logging.info("End up " + network_name)
                
            except e:
                logging.error(f"Error: {e}")

    def bridge_down(self, edge: Edge):
        u, v = edge
        if u > v:
            u, v = v, u
        network_name = f"net_{u}_{v}"
        
        logging.info(f"Down {network_name}")

        if network_name in self.bridges.keys():
            try:
                container_u = self.containers.get(u)
                container_v = self.containers.get(v)
                ip_u = container_u.attrs['NetworkSettings']['Networks'][network_name]['IPAddress']
                ip_v = container_v.attrs['NetworkSettings']['Networks'][network_name]['IPAddress']
                
                print(container_u.exec_run(f'ip link set {self.get_interface_from_container(container_u, ip_u)} down'))
                print(container_v.exec_run(f'ip link set {self.get_interface_from_container(container_v, ip_v)} down'))
                
                self.shapers.pop(network_name)
            except e:
                logging.error(f"Error: {e}")

    def create_devices(self, num_containers):
        logging.info(f"Creating {num_containers} devices...")

        if not os.path.exists(self.binary):
            logging.error(f"Binary not found at: {self.binary}")
            sys.exit(1)

        try:
            self.docker_client.images.get(BASE_IMAGE)
        except docker.errors.ImageNotFound:
            logging.info(f"Building emulator image...")
            shell(f"sudo docker build . -t {BASE_IMAGE}")
        except Exception as e:
            logging.error(f"Error checking/pulling base image: {e}")
            sys.exit(1)
            
        def create_container(i):
            container_name = f"dist_node_{i}"
            try:
                container = self.docker_client.containers.run(
                    BASE_IMAGE,
                    name=container_name,
                    hostname=f"node{i}",
                    read_only=False,
                    detach=True,
                    network=None,
                    network_mode="bridge",
                    tty=True,
                    privileged=True,
                    cap_add=["NET_ADMIN"],
                    stdin_open=True,
                    volumes={
                        self.binary: {
                            "bind": f"/work/{self.binary.name}",
                            "mode": "ro",
                        },
                        self.logs: {
                            "bind": "/work",
                            "mode": "rw"
                        }
                    },
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
                    f"Error creating container {container_name}: {e}")
                self.cleanup()
                sys.exit(1)
            
        with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
            executor.map(create_container, range(1, num_containers + 1))

        logging.info(
            "Disconnecting containers from the default bridge network...")

        default_bridge_name: str = None

        for name in ["bridge", "docker0"]:
            try:
                bridge_network = self.docker_client.networks.get(name)
                default_bridge_name = name
                break
            except Exception:
                logging.info(
                    f"Default bridge network '{default_bridge_name}' not found."
                )

        if not default_bridge_name:
            logging.error("Error getting default bridge network")
            return

        try:
            for container in self.containers.values():
                try:
                    logging.info(
                        f"Disconnecting {container.name} from network '{default_bridge_name}'..."
                    )
                    bridge_network.disconnect(container)
                    logging.info(
                        f"Successfully disconnected {container.name} from '{default_bridge_name}'."
                    )
                except Exception as e:
                    logging.error(
                        f"Unexpected error disconnecting {container.name} from '{default_bridge_name}': {e}"
                    )
        except Exception as e:
            logging.error(
                f"Unexpected error using default bridge network: {e}. Skipping disconnection step."
            )

    def run_experiment(self, config: ExperimentConfig):
        self.create_devices(config.init.nodes_amount)

        os.mkdir(self.logs / 'logs')

        def run_binary_in_device(i: int):
            container = self.containers.get(i)
            if not container:
                logging.warning(
                    f"Container {i} not found, skipping binary start.")
                return

            try:
                container.exec_run(cmd=f"touch /work/logs/log_{i}")
                exec_cmd = f'bash -c "/work/{self.binary.name} {i} {config.setup.duration} > /work/logs/log_{i} 2>&1"'
                exit_code, _ = container.exec_run(cmd=exec_cmd,
                                                  user="root",
                                                  detach=True)

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

        # Setup bridges
        
        self.start_time = None
        
        def_props = EdgeProperties(
            conn_lat=config.setup.conn_latency,
            lat=config.setup.latency,
            bw=config.setup.bandwidth
        )
        
        bridges = dict()
        for edge in config.init.edges:            
            bridges[edge] = def_props
            
        for mod in config.mods:
            for edge in mod.add:
                bridges[edge] = def_props
                
        for edge, props in config.init.custom.items():
            bridges[edge] = props
        
        with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
            executor.map(lambda x: self.add_bridge(x[0], x[1]), bridges.items())
            
        with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
            executor.map(self.bridge_up, config.init.edges)
        
        with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
            executor.map(run_binary_in_device,
                         list(range(1, config.init.nodes_amount + 1)))
        
        # Mods
        asyncio.run(self.run_exp(config))
        
    async def run_exp(self, config):
        
        self.start_time = datetime.now()
        
        async def delayed_task(task, delay):
            await asyncio.sleep(delay / 1000)
            await task()
            
        tasks: List[asyncio.Task] = []
        for mod in config.mods:
            tasks.append(
                asyncio.create_task(delayed_task(partial(self.make_modification, mod), mod.time)))

        await asyncio.sleep(config.setup.duration)

        for task in tasks:
            task.cancel()

    async def make_modification(self, mods: ExperimentMods):
        print("Mod start:", (datetime.now() - self.start_time).total_seconds())
        
        with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
            for edge in mods.add:
                executor.submit(self.bridge_up, edge)
            for edge in mods.erase:
                executor.submit(self.bridge_down, edge)
  
        print("Mod end:", (datetime.now() - self.start_time).total_seconds())
        

    def cleanup(self):
        logging.info("Cleaning up resources...")
        
        with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
            for _, shaper in self.shapers.items():
                executor.submit(shaper.clear_rules)
        
        logging.info(shell('docker rm -f $(docker ps -aq --filter "name=dist_node_")'))
        logging.info(shell('docker network ls --filter name=net_ --format "{{.ID}}" | xargs -P 4 -n 1 -r docker network rm'))

        logging.info("Cleanup complete.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description=
        "Deploy a distributed system in Docker containers based on a config file."
    )
    parser.add_argument("-b",
                        "--binary-path",
                        help="Path to algorithm binary.",
                        required=True)
    parser.add_argument("-c",
                        "--config-file",
                        help="Path to the experiment configuration file.",
                        required=True)
    parser.add_argument("-l",
                        "--logs-path",
                        help="Path to directory for retreiving logs.",
                        required=True)

    args = parser.parse_args()

    if shell("cat /proc/sys/net/ipv4/ip_forward").stdout.decode(
            'utf-8') != '1':
        logging.info('Enabling ip forwarding')
        shell("echo 1 > /proc/sys/net/ipv4/ip_forward")
        shell("echo 1 > /proc/sys/net/ipv4/conf/all/bc_forwarding")

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
    
    os.mkdir(logs_num_path)

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

        logging.info('Combine logs')
        combine_logs.main(logs_num_path / 'logs', logs_num_path, experiment_config.setup.duration)
