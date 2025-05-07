import os
import argparse
import sys
from collections import defaultdict
from pathlib import Path, PurePath

parsed_list = []

config_name = "config.txt"

mac_to_code = dict()

NS3_START_TIME = 1000


def parse_solo(solo_text: list[str], dir_name: str):
    if len(solo_text) == 0:
        print("Incorrect data in .dot file")
        sys.exit(1)

    header = solo_text[0].split()[1]  # remove 'digraph' keyword and bracket
    solo_text = solo_text[1:-1]  # remove header and closing bracket
    timestamp = int(header.split("_")[-1])
    uid = header.split("_")[1]
    timestamp = int(header.split("_")[-1])
    uid = header.split("_")[1]

    visible_nodes = {}
    visible_edges = set()
    visible_edges_valid = defaultdict(int)

    config_edges = set()

    for line in solo_text:
        line = line.strip()
        if "->" not in line:
            words = line.split()
            short = words[0].strip()
            long = words[4].strip(' "]')
            visible_nodes[short] = long
        else:
            from_node, to_node = list(map(str.strip, line.split("->")))
            to_node, col = to_node.split(" [")
            col = col.split("=")[1][:-1]

            edge = (visible_nodes[from_node], visible_nodes[to_node])

            if col == '"blue"':
                config_edges.add(edge)
                continue

            visible_edges_valid[edge] += 1
            if visible_edges_valid[edge] < 2:
                continue
            elif visible_edges_valid[edge] == 2:
                visible_edges.add(edge)
            else:
                print("Something is wrong in .dot file, edge appear 3 times")
                sys.exit(1)

    return (
        timestamp,
        uid,
        list(visible_nodes.values()),
        list(visible_edges),
        dir_name,
        list(config_edges),
    )


# all discovery edges
configuration = set()


def print_header(file):
    printf = lambda cur_text: print(cur_text, file=file)
    sorted_mac_to_code = dict(sorted(mac_to_code.items(), key=lambda item: item[1]))
    for key in sorted_mac_to_code.keys():
        printf(f"0 {key} {mac_to_code[key]}")


def pretty_parse_logs(cur_logs, min_time: int):

    has_dynamic: bool = False

    curr_nodes = []
    curr_edges = []
    curr_config = []

    file_content = []

    for cur_log in cur_logs:

        timestamp = cur_log[0] - min_time
        new_nodes = cur_log[1]
        new_edges = cur_log[2]
        new_config = cur_log[3]

        config_diff = (set(new_config) - set(curr_config)).union(
            set(curr_config) - set(new_config)
        )

        if len(config_diff) > 0 and not has_dynamic:  # type: ignore
            has_dynamic = True

        for config in config_diff:
            if config not in curr_config:
                curr_config.append(config)
                file_content.append(f"5 {timestamp} {config[0]} {config[1]}")
                configuration.add(
                    (timestamp, min(config[0], config[1]), max(config[0], config[1]), 1)
                )

            elif config not in new_config:
                curr_config.remove(config)
                file_content.append(f"6 {timestamp} {config[0]} {config[1]}")
                configuration.add(
                    (timestamp, min(config[0], config[1]), max(config[0], config[1]), 0)
                )

        node_diff = (set(new_nodes) - set(curr_nodes)).union(
            set(curr_nodes) - set(new_nodes)
        )
        for node in node_diff:
            if node not in curr_nodes:
                curr_nodes.append(node)
                file_content.append(f"1 {timestamp} {node}")
            elif node not in new_nodes:
                curr_nodes.remove(node)
                file_content.append(f"3 {timestamp} {node}")

        edge_diff = (set(new_edges) - set(curr_edges)).union(
            set(curr_edges) - set(new_edges)
        )
        for edge in edge_diff:
            if edge not in curr_edges:
                curr_edges.append(edge)
                file_content.append(f"2 {timestamp} {edge[0]} {edge[1]}")
            elif edge not in new_edges:
                curr_edges.remove(edge)
                file_content.append(f"4 {timestamp} {edge[0]} {edge[1]}")

    return file_content


def process_solo(cur_logs, min_time: int):
    clean_logs = list(map(lambda t: (t[0], t[2], t[3], t[5]), cur_logs))
    clean_logs = sorted(clean_logs, key=lambda x: x[0])
    return pretty_parse_logs(clean_logs, min_time)


def mac_to_node_id(mac):
    cur_sz = len(mac_to_code)
    if not mac in mac_to_code.keys():
        cur_sz += 1  # type: ignore
        mac_to_code[mac] = cur_sz

    return mac_to_code[mac]


def clear_config():
    visibility = sorted(configuration)  # type: ignore
    results = []

    for i in range(len(visibility)):
        ok_del = False
        for j in range(len(results) - 1, -1, -1):
            f1 = visibility[i][1]
            t1 = visibility[i][2]

            if f1 < t1:
                f1, t1 = t1, f1

            f2 = results[j][1]
            t2 = results[j][2]

            if f2 < t2:
                f2, t2 = t2, f2

            if f1 == f2 and t1 == t2 and visibility[i][3] == visibility[j][3]:
                ok_del = True
                break
        if not ok_del:
            results.append(visibility[i])
    return results


def parse_all_nodes(macs):

    cleared_configuration = clear_config()
    present_macs = list(macs)
    pretty_events = []
    app_off_events = []
    for event in cleared_configuration:
        event = list(event)

        if not event[1] in present_macs:
            app_off_events.append([NS3_START_TIME, mac_to_node_id(event[1])])
            present_macs.append(event[1])

        if not event[2] in present_macs:
            app_off_events.append([NS3_START_TIME, mac_to_node_id(event[2])])
            present_macs.append(event[2])

        event[1] = mac_to_node_id(event[1])
        event[2] = mac_to_node_id(event[2])
        pretty_events.append(event)
    return pretty_events, app_off_events


def create_config_file(graph_dir, pretty_events, app_off_events):

    config_path = f"{os.path.dirname(graph_dir)}/{config_name}"
    print("Generated config: ", config_path)
    f = open(config_path, mode="w+")

    n = len(mac_to_code)

    f.write(f"{n}\n")
    for i in range(1, n + 1):
        f.write(f"{i}:\n")

    for event in app_off_events:
        f.write(f"{int(event[0]) + 1} app {event[1]} 0\n")
    for event in pretty_events:
        f.write(
            f"{int(event[0]) // 1000 + NS3_START_TIME} link {event[1]} {event[2]} {event[3]}\n"
        )
    f.close()


if __name__ == "__main__":

    parser = argparse.ArgumentParser(
        prog="combine_logs.py",
        description="Create files for visualizator from .dot files",
    )

    parser.add_argument(
        "-p",
        "--path-to-visualization",
        help="path to dir with .dot files.",
        required=True,
    )

    parser.add_argument(
        "-o",
        "--path-to-output",
        help="path to directory (may be non-existing) to put output.",
        required=True,
    )

    parser.add_argument(
        "-g",
        "--generate-config",
        action="store_true",
        help="generate config.txt file from lsdb. File will be stored in parent directory for path-to-visualization.",
        required=False,
    )

    args = parser.parse_args()

    graph_dir = Path(os.path.abspath(args.path_to_visualization))
    if not os.path.exists(graph_dir):
        print("Directory " + str(graph_dir) + " does not exist.")
        sys.exit(1)

    pathdir = graph_dir.glob("*.dot")
    logs = []
    logs_by_uid = defaultdict(list)
    for path in pathdir:
        cur_dir = PurePath(os.path.basename(path).split("-")[1]).stem
        with open(path) as fd:
            full_text = fd.readlines()
            text = []
            for line in full_text:
                text.append(line)
                if "}" in line:
                    logs.append(parse_solo(text, cur_dir))
                    text = []
    logs = sorted(logs, key=lambda x: x[0])
    for log in logs:
        logs_by_uid[log[1]].append(log)

    min_time = 10**18
    for key, value in logs_by_uid.items():
        for graph in value:
            min_time = min(min_time, graph[0])

    print(f"Normalizing time in logs from {min_time // 1000} ms")

    # save main log for each UID + enumerate present nodes
    files_content = {}
    log_cnt = 1
    for key, value in logs_by_uid.items():
        dir_name = value[0][4]
        if not dir_name.isdigit():
            dir_name = mac_to_node_id(key)
        else:
            mac_to_code[key] = int(dir_name)
        files_content[(key, dir_name)] = process_solo(value, min_time)  # type: ignore

    # prepare events for config + enumerate non-present nodes
    channel_events, off_events = parse_all_nodes(logs_by_uid.keys())

    output_dir = os.path.abspath(args.path_to_output)
    Path(output_dir).mkdir(parents=True, exist_ok=True)

    for key, value in files_content.items():
        filename = f"{key[1]}_{key[0]}.txt"
        with open(f"{output_dir}/{filename}", "w") as FD:
            print_header(FD)
            for line in value:
                FD.write(line + "\n")
        parsed_list.append(filename)

    if args.generate_config:
        create_config_file(output_dir, channel_events, off_events)

    print(f"Parsed successfully. Check results in {output_dir}.\nParsed files are:")
    print(parsed_list)
