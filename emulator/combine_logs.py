from dataclasses import dataclass
import os
from pathlib import Path
import re
import argparse
from datetime import datetime


@dataclass
class Event:
    mac1: str
    what: str
    mac2: str


def combine_logs(logs_path, duration):
    mac_to_num = {}
    all_events = []
    
    print(duration)

    start_pattern = re.compile(
        r"\[(.*?)\]IpTransport:INFO:Start:IP transport started with pid: ([\dA-Fa-f:]{17})"
    )
    neighbor_found_pattern = re.compile(
        r"\[(.*?)\]IpTransport:INFO:OnNeighborFound:Found new neighbor: ([\dA-Fa-f:]{17})"
    )
    neighbor_lost_pattern = re.compile(
        r"\[(.*?)\]IpTransport:INFO:OnNeighborLost:Lost neighbor: ([\dA-Fa-f:]{17})"
    )
    link_establish_pattern = re.compile(
        r"\[(.*?)\]IpTransport:INFO:OnLinkEstablishing:(INCOMING|OUTGOING) session opened with peer ([\dA-Fa-f:]{17})"
    )
    link_closed_pattern = re.compile(
        r"\[(.*?)\]IpTransport:INFO:OnLinkClosed:(INCOMING|OUTGOING) session closed with peer ([\dA-Fa-f:]{17})"
    )
    
    time_start: datetime = None
    
    for filename in sorted(os.listdir(logs_path)):
        if not filename.startswith('log_'):
            continue
        
        log_start: datetime = None
        file_path = os.path.join(logs_path, filename)

        with open(file_path, 'r') as f:
                    
            for line in f:
                line = line.strip()
                start_match = start_pattern.match(line)
                if start_match:
                    log_start = datetime.strptime(start_match.group(1), "%H:%M:%S.%f")
                    break
                
        if not log_start:
            print(f'In log {filename} IP transport was not started')
        elif not time_start or log_start < time_start:
            time_start = log_start
        
    if not time_start:
        print(f'IP transport was not started anywhere not started')
        return {}, {}

    for filename in sorted(os.listdir(logs_path)):
        if not filename.startswith('log_'):
            continue

        log_num = filename.split('_')[1].split('.')[0]
        file_path = os.path.join(logs_path, filename)
        mac_address = None

        with open(file_path, 'r') as f:
                    
            for line in f:
                line = line.strip()

                if not mac_address:
                    start_match = start_pattern.match(line)
                    if start_match:
                        mac_address = start_match.group(2).upper()
                        print('Found: ', mac_address)
                        mac_to_num[mac_address] = log_num
                    continue

                event = None
                time_str = None

                if 'OnNeighborFound' in line:
                    match = neighbor_found_pattern.match(line)
                    if match:
                        time_str = match.group(1)
                        neighbor_mac = match.group(2).upper()
                        event = Event(mac1=mac_address,
                                      what="saw",
                                      mac2=neighbor_mac)
                elif 'OnNeighborLost' in line:
                    match = neighbor_lost_pattern.match(line)
                    if match:
                        time_str = match.group(1)
                        neighbor_mac = match.group(2).upper()
                        event = Event(mac1=mac_address,
                                      what="lost",
                                      mac2=neighbor_mac)
                elif 'OnLinkEstablishing' in line:
                    match = link_establish_pattern.match(line)
                    if match:
                        time_str = match.group(1)
                        direction = match.group(2)
                        peer_mac = match.group(3).upper()
                        event = Event(mac1=mac_address,
                                      what=f"open {direction}",
                                      mac2=peer_mac)
                elif 'OnLinkClosed' in line:
                    match = link_closed_pattern.match(line)
                    if match:
                        time_str = match.group(1)
                        direction = match.group(2)
                        peer_mac = match.group(3).upper()
                        event = Event(mac1=mac_address,
                                      what=f"close {direction}",
                                      mac2=peer_mac)

                if event and time_str and time_start:
                    try:
                        timestamp = datetime.strptime(time_str, "%H:%M:%S.%f") - time_start
                        if timestamp.total_seconds() < duration:
                            all_events.append((timestamp, event))
                    except ValueError:
                        continue

    all_events.sort(key=lambda x: x[0])

    return mac_to_num, all_events

class Graph:
    def __init__(self):
        self.graph = {}
    
    def add_vertex(self, vertex):
        if vertex not in self.graph:
            self.graph[vertex] = []
    
    def add_edge(self, u, v):
        self.add_vertex(u)
        self.add_vertex(v)
        self.graph[u].append(v)
        self.graph[v].append(u)
        
    def erase_edge(self, u, v):
        self.add_vertex(u)
        self.add_vertex(v)
        self.graph[u].remove(v)
        self.graph[v].remove(u)
    
    def is_connected(self):
        if not self.graph:
            return True        
        visited = set()
        start_vertex = next(iter(self.graph))
        stack = [start_vertex]
        
        while stack:
            vertex = stack.pop()
            if vertex not in visited:
                visited.add(vertex)
                for neighbor in self.graph.get(vertex, []):
                    if neighbor not in visited:
                        stack.append(neighbor)
        
        return len(visited) == len(self.graph)
    
    def __eq__(self, other):
        return self.graph == other.graph

def get_experiment_stats(mac_to_num, all_events):
    
    graph = Graph()
    
    for mac, num in mac_to_num.items():
        graph.add_vertex(num)
    
    incoming = set()
    outgoing = set()
    
    visible = set()
    
    conn_time: str = None
    sync_time: str = None
    
    last_state = graph.is_connected()
    
    events = []
    
    for it in all_events:
        time = it[0]
        event = it[1]
        
        new_state = graph.is_connected()
        
        a = mac_to_num[event.mac1]
        b = mac_to_num[event.mac2]
        
        if "saw" in event.what:
            visible.add((a, b))
            if (b, a) in visible:
                events.append((time, f"{b} <> {a}"))
                
        if "lost" in event.what:
            visible.remove((a, b))
            if (b, a) in visible:
                events.append((time, f"{b} >< {a}"))
        
        if "open" in event.what:    
            if "INCOMING" in event.what:
                incoming.add((a, b))
                if (b, a) in outgoing:
                    events.append((time, f"{b} -> {a}"))
                    graph.add_edge(a, b)
            if "OUTGOING" in event.what:
                outgoing.add((a, b))
                if (b, a) in incoming:
                    events.append((time, f"{a} -> {b}"))
                    graph.add_edge(a, b)
            
            new_state = graph.is_connected()
                    
        if "close" in event.what:    
            if "INCOMING" in event.what:
                incoming.remove((a, b))
                if (b, a) in outgoing:
                    events.append((time, f"{b} |> {a}"))
                    graph.erase_edge(a, b)
            if "OUTGOING" in event.what:
                outgoing.remove((a, b))
                if (b, a) in incoming:
                    events.append((time, f"{a} |> {b}"))
                    graph.erase_edge(a, b)
                    
            new_state = graph.is_connected()

        if last_state == False and new_state == True:
            conn_time = time
            
        last_state = new_state
        
        if last_state == True:
            sync_time = time
        else:
            conn_time = None
            sync_time = None
            
    return conn_time, sync_time, events
        


def main(logs_path, output_dir, duration):

    mac_to_num, all_events = combine_logs(logs_path, duration)

    with open(output_dir / 'combined_log.txt', 'w') as f:
        for mac, num in sorted(mac_to_num.items(), key=lambda x: int(x[1])):
            f.write(f"{num}: {mac}\n")
        f.write("\n")
        for ev in all_events:
            event = ev[1]
            f.write(f"[{ev[0]}] {mac_to_num[event.mac1]} {event.what} {mac_to_num[event.mac2]}\n")

    print(f"Successfully created combined log at: {output_dir / 'graph.txt'}")
    
    conn_time, sync_time, events = get_experiment_stats(mac_to_num, all_events)
    
    with open(output_dir / 'graph.txt', 'w') as f:
        for mac, num in sorted(mac_to_num.items(), key=lambda x: int(x[1])):
            f.write(f"{num}: {mac}\n")
        f.write("\n")
        for ev in events:
            f.write(f"[{ev[0]}] {ev[1]}\n")
    
    if conn_time:
        print("Time to connect topology:", conn_time.total_seconds())
        print("Time to synchronize topology:", sync_time.total_seconds())
    else:
        print("Topology is not connected")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='Combine emulator log files', )
    parser.add_argument('-l',
                        '--logs-path',
                        help='Path to directory containing log files',
                        required=True)
    parser.add_argument('-o',
                        '--output_dir',
                        help='Path to output dir for combined log file',
                        required=True)
    
    parser.add_argument('-d',
                        '--duration',
                        help='Experiment duration',
                        required=True)

    args = parser.parse_args()

    if not os.path.isdir(args.logs_path):
        print(f"Error: Directory '{args.logs_path}' does not exist!")
        exit(1)

    logs_path = Path(args.logs_path).absolute()
    output_dir = Path(args.output_dir).absolute()
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    main(logs_path, output_dir, int(args.duration))
