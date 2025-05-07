import os
import re
import sys
import argparse

from os import listdir
from os.path import isfile, join
from typing import Optional

from common import Codes, Colours, colstr

ENABLE_LOGS: bool = False

# constraint constants
MAX_FINAL_EDGE_COUNT: int = 3
MAX_MED_EDGE_COUNT = 5
MAX_DIAMETER: int = 4
INF = int(1e9 + 7)


def Floyd(matrix, n):
    for k in range(n):
        for i in range(n):
            for j in range(n):
                matrix[i][j] = min(matrix[i][j], matrix[i][k] + matrix[k][j])
    return matrix


def get_graph_diameter(matrix, n):
    diameter = 0
    for i in range(n):
        for j in range(n):
            diameter = max(diameter, matrix[i][j])
    return diameter


def create_connectivity_matrix(graph, n):
    matrix = [[0 if i == j else INF for i in range(n)] for j in range(n)]
    mp: dict = {}
    i = 0
    for node in graph:
        mp[node] = i
        i += 1
    for node in graph:
        for neighbour in graph[node]:
            matrix[mp[node]][mp[neighbour]] = 1
    return matrix


def count_diameter(graph, n):
    matrix = create_connectivity_matrix(graph, n)
    return get_graph_diameter(Floyd(matrix, n), n)


def count_neighbours(neighbours):
    neigh_dict = {}
    for i in neighbours:
        if not i in neigh_dict:
            neigh_dict[i] = 0
        neigh_dict[i] += 1
    cnt = 0
    for i in neigh_dict.keys():
        if int(neigh_dict[i]) >= 2:  # 2 or 3 -- first session
            cnt += 1
        if int(neigh_dict[i]) == 4:  # 4 -- second session
            cnt += 1
    return cnt


def count_all_max_edges_count(graph_all_files, mac_to_id):

    problem_med_edges = dict()
    problem_final_edges = dict()

    for i in range(len(graph_all_files)):

        code = graph_all_files[i][0][1]
        cur_node_id = mac_to_id[graph_all_files[i][0][2][code]] + 1
        final_time = get_time_from_name(graph_all_files[i][-1][0])

        for graph_info in graph_all_files[i]:

            graph = graph_info[3]
            root = graph_info[1]
            current_time = get_time_from_name(graph_info[0])

            if root in graph.keys():

                neighbour_cnt = count_neighbours(graph[root])

                if current_time == final_time and neighbour_cnt > MAX_FINAL_EDGE_COUNT:
                    problem_final_edges[cur_node_id] = neighbour_cnt

                elif neighbour_cnt > MAX_MED_EDGE_COUNT:

                    if not cur_node_id in problem_med_edges.keys():
                        problem_med_edges[cur_node_id] = []

                    problem_med_edges[cur_node_id].append((current_time, neighbour_cnt))

    return problem_final_edges, problem_med_edges


def dfs_component(x: str, graph, component: set):
    # print(x, component)
    component.add(x)
    # neighbour must be stored 2 times if we have session
    if not x in graph.keys():
        return
    real_neighbours = [i for i in graph[x] if graph[x].count(i) >= 2]
    for i in real_neighbours:
        if not i in component:
            dfs_component(i, graph, component)


def get_connectivity_component(x: str, graph):
    component = set()
    dfs_component(x, graph, component)
    return component


def get_time_from_name(name: str):
    return int(name.split("_")[-1])


def parse_graphs(fd, macs):
    graphs = []
    text = fd.readlines()
    tmp_conn_graph = {}
    tmp_vis_graph = {}
    nodes: dict = {}
    name = ""
    root = ""
    i = 0
    for line in text:
        i += 1
        if line.count("}") > 0:
            graphs.append((name, root, nodes, tmp_conn_graph, tmp_vis_graph))
        elif line.count("digraph") > 0:
            tmp_conn_graph = {}
            tmp_vis_graph = {}
            name = line.split()[1]
            nodes: dict = {}
        elif line.count("->") > 0:
            from_node, to_node = list(map(str.strip, line.split("->")))
            to_node, col = to_node.split(" [")
            col = col.split("=")[1][:-1]

            nds = [from_node, to_node]
            if nds[0] not in nodes.keys() or nds[1] not in nodes.keys():
                continue

            if col == '"red"':  # connection edge
                if nds[0] not in tmp_conn_graph:
                    tmp_conn_graph[nds[0]] = []
                if nds[1] not in tmp_conn_graph:
                    tmp_conn_graph[nds[1]] = []
                tmp_conn_graph[nds[0]].append(nds[1])
                tmp_conn_graph[nds[1]].append(nds[0])
            else:  # visibility edge
                if nds[0] not in tmp_vis_graph:
                    tmp_vis_graph[nds[0]] = []
                if nds[1] not in tmp_vis_graph:
                    tmp_vis_graph[nds[1]] = []
                # directed graph
                tmp_vis_graph[nds[0]].append(nds[1])
                # print(f"{name} from {nds[0]} to {nds[1]} -> {tmp_vis_graph}")

        else:
            nds = re.findall(r"a\d+", line)
            if len(nds) > 0:
                if line.count("red") > 0:
                    root = nds[0]
                lbl = re.findall(r'label = "..:..:..:..:..:.."', line)
                if len(lbl) > 0:
                    label = re.findall(r"..:..:..:..:..:..", lbl[0])[0]
                    if label in macs:
                        nodes[nds[0]] = label

    return graphs


def graph_nodes_to_macs(nodes, edges):
    nedges = {}
    for node, peers in edges.items():
        nedges[nodes[node]] = []

        for peer in peers:
            nedges[nodes[node]].append(nodes[peer])
        nedges[nodes[node]] = sorted(nedges[nodes[node]])
    return nedges


def equal_lists(l1, l2):
    return (
        len(l1) == len(l2) and len([i for i in range(len(l1)) if l1[i] != l2[i]]) == 0
    )


def equal_dicts(g1, g2):
    for key, value in g1.items():
        if not key in g2.keys():
            # print(f"key {key} is not in g2")
            return False
        for v in value:
            if not v in g2[key] or g2[key].count(v) != value.count(v):
                # print(f"v {v} is not in g2[key]")
                return False
    return True


def dfs1_csv(x, graph, used, order):
    used[x] = True
    for i in graph[x]:
        if not used[i]:
            dfs1_csv(i, graph, used, order)
    order.append(x)


def dfs2_csv(x, graph, used, components):
    used[x] = True
    components.append(x)
    if not x in graph.keys():
        return
    for i in graph[x]:
        if not used[i]:
            dfs2_csv(i, graph, used, components)


def bin_search(graphs, x):
    l = 0
    r = len(graphs)
    while r - l > 1:
        m = (r + l) // 2
        cur_time = get_time_from_name(graphs[m][0])
        if cur_time > x:
            r = m
        else:
            l = m
    return l


def reset_used(g, gr):

    used = {i: False for i in gr.keys()}
    for i in g.keys():
        if not i in used.keys():
            used[i] = False

    return used


def find_strong_components(graph):
    graph_reversed = {}

    for key, value in graph.items():
        for j in value:
            if not j in graph_reversed.keys():
                graph_reversed[j] = []
            graph_reversed[j].append(key)

    used = reset_used(graph, graph_reversed)

    order = []
    for i in graph.keys():
        if not used[i]:
            dfs1_csv(i, graph, used, order)
    used = reset_used(graph, graph_reversed)
    order = reversed(order)

    components = []
    for i in order:
        if not used[i]:
            component = []
            dfs2_csv(i, graph_reversed, used, component)
            components.append(sorted(component))
    return sorted(components)


def cmp_views(g1, g2):
    nodes1 = sorted([g1[2][i] for i in g1[2].keys()])
    nodes2 = sorted([g2[2][i] for i in g2[2].keys()])

    ok_nodes = equal_lists(nodes1, nodes2) and equal_lists(nodes2, nodes1)

    edges1 = graph_nodes_to_macs(g1[2], g1[3])
    edges2 = graph_nodes_to_macs(g2[2], g2[3])

    ok_edges = equal_dicts(edges1, edges2) and equal_dicts(edges2, edges1)

    vis_edges1 = graph_nodes_to_macs(g1[2], g1[4])
    vis_edges2 = graph_nodes_to_macs(g2[2], g2[4])

    ok_vis = equal_dicts(vis_edges1, vis_edges2) and equal_dicts(vis_edges2, vis_edges1)

    return ok_nodes and ok_edges and ok_vis


def check_all_pairs_synchronization(
    graph_files, end_time, cc_component, mac_to_id, cc_sync_time
):

    # print(f"till min time {end_time}")
    for peer_id_1 in range(len(cc_component)):
        for peer_id_2 in range(peer_id_1 + 1, len(cc_component)):

            pair_sync_time = 0

            peer1 = cc_component[peer_id_1]
            peer2 = cc_component[peer_id_2]

            peer1_graphs = graph_files[mac_to_id[peer1]]
            peer2_graphs = graph_files[mac_to_id[peer2]]

            has_eq_views = False

            # print(f"check: {peer1} ({mac_to_id[peer1]}) {peer2} ({mac_to_id[peer2]})")

            min_graph1_id = bin_search(peer1_graphs, end_time)
            min_graph2_id = bin_search(peer2_graphs, end_time)

            i2 = len(peer2_graphs) - 1
            i1 = len(peer1_graphs) - 1
            while i1 >= min_graph1_id and i2 >= min_graph2_id:

                time1 = get_time_from_name(peer1_graphs[i1][0])
                time2 = get_time_from_name(peer2_graphs[i2][0])

                # print(f"cmp {i1} -> {peer1} ({time1}), {i2} -> {peer2} ({time2})")

                same_views = cmp_views(peer1_graphs[i1], peer2_graphs[i2])

                if same_views:
                    # print("ok")
                    has_eq_views = True
                    pair_sync_time = max(time1, time2)

                else:
                    break

                if time1 > time2:
                    i1 -= 1
                else:
                    i2 -= 1

            if not has_eq_views:
                # print(f"bad pair {peer1} {peer2}")
                return False
            # print(f"{peer1} {peer2} sync in {pair_sync_time}")
            cc_sync_time.append(pair_sync_time)

    return True


def check_topology_synchronization(
    graph_files, last_connectivity_component, mac_to_id, sync_timestamps
):

    # can not have more cc than nodes
    used = [False for i in range(len(graph_files))]

    for i in range(len(graph_files)):

        if used[i]:
            continue

        cc_component = last_connectivity_component[i][0]  # type: ignore
        cc_graph_id = last_connectivity_component[i][1]
        # print("graph id is ", cc_graph_id)
        fixed_time = get_time_from_name(graph_files[i][cc_graph_id][0])

        if len(cc_component) == 1:
            sync_timestamps.append(fixed_time)
            # print("Component size is 1, no objects for sync")
            continue

        cc_sync_time = []

        if not check_all_pairs_synchronization(
            graph_files, fixed_time, cc_component, mac_to_id, cc_sync_time
        ):
            print(f"component {cc_component} is not synchronized")
            return False
        else:
            sync_timestamps.append(max(cc_sync_time))
            for peer in cc_component:
                used[mac_to_id[peer]] = True

    return True


def check_csv_and_cc(cc_component, csv_component, err_str):

    if len(cc_component) < len(csv_component):
        err_str += f"Conectivity component has less nodes than csv {cc_component}\n{csv_component}"
        return False

    for node in csv_component:
        if not node in cc_component:
            err_str += f"Node {node} in cc "
            return False
    return True


def check_topology_connectivity(
    graph_files, last_csv_components, connectivity_timestamps, last_cc_components
):

    for i in range(len(graph_files)):

        graphs = graph_files[i]
        # Last csv  graph for current node
        last_csv = last_csv_components[i]
        csv_component = last_csv[1][last_csv[3]]

        has_connectivity_component = False
        err_str = ""
        connectity_time = -1

        for j in range(len(graphs) - 1, last_csv[2] - 1, -1):
            g = graphs[j]

            cc_component = get_connectivity_component(g[1], g[3])

            if check_csv_and_cc(cc_component, csv_component, err_str):
                has_connectivity_component = True
                connectity_time = get_time_from_name(g[0])
                last_cc_components[i] = ([g[2][node] for node in cc_component], j)  # type: ignore aaaa
            else:
                break

        if not has_connectivity_component:
            sys.stderr.write(err_str)
            return False
        else:
            connectivity_timestamps.append(connectity_time)
    # print("last cc: ", last_cc_components)
    return True


def make_visibiity_graph(nodes, disc_edges):
    vis_graph = {}
    for key in nodes.keys():
        vis_graph[key] = []

    for key, value in disc_edges.items():
        vis_graph[key] = value

    return vis_graph


def make_csv_components(graph_files, last_csv_components):

    for i in range(len(graph_files)):
        graphs = graph_files[i]
        # prev csv for this graph
        last_csv = (0, [], 0, -1)

        for j in range(len(graphs)):
            g = graphs[j]
            fixed_time = get_time_from_name(g[0])
            visibility_graph = make_visibiity_graph(g[2], g[4])
            csv = find_strong_components(visibility_graph)
            root = g[1]

            csv_id = -1
            for peer1 in range(len(csv)):
                cc_component = csv[peer1]
                if root in cc_component:
                    csv_id = peer1
                    break
            if csv_id == -1:
                # continue
                sys.stderr.write("Node has no scv, but must be in it's own\n")
                return Codes.FAIL

            if not csv == last_csv[1]:
                last_csv = (fixed_time, csv, j, csv_id)

        last_csv_components.append(last_csv)
    return Codes.OK


def get_min_time(graph_files):
    mn_time: Optional[int] = None
    for graphs in graph_files:
        for g in graphs:
            if mn_time is None or mn_time > get_time_from_name(g[0]):
                mn_time = get_time_from_name(g[0])
    return mn_time


def print_connectivity_info(mn_time, connectivity_ok, connectivity_timestamps):

    print("1. Time to make one connectivity component: ", end="", flush=True)
    if not connectivity_ok:
        print(colstr("failed to form one connectivity component.", Colours.RED))
    else:
        connect_time = (max(connectivity_timestamps) - mn_time) / 1000
        print(f"{connect_time} ms.")
        for i in range(len(connectivity_timestamps)):
            print(
                f"Node {i + 1} time = {(connectivity_timestamps[i] - mn_time) / 1000} ms"
            )


def print_synchronization_info(mn_time, sync_ok, synchronization_timestamps):

    print("2. Time to synchronize the topology: ", end="", flush=True)
    if not sync_ok:
        print(colstr("topology is not synchronized.\n", Colours.RED))
    else:
        sync_time = (max(synchronization_timestamps) - mn_time) / 1000
        print(f"{sync_time} ms.")

        for i in range(len(synchronization_timestamps)):
            print(
                f"Comp {i + 1} time = {(synchronization_timestamps[i] - mn_time) / 1000} ms"
            )


def check_topology_metrics(graph_files, mac_to_id) -> Codes:

    diametres = set()

    for i in range(len(graph_files)):
        graph_for_metrics = graph_files[i][-1][3]
        diametres.add(count_diameter(graph_for_metrics, len(graph_files)))

    print("Graph components diameter is ", end="")
    for i in diametres:
        print(i, end="")
    print()

    edges_final, edges_med = count_all_max_edges_count(graph_files, mac_to_id)

    if len(edges_final) > 0:
        sys.stderr.write(f"Node edges: (limit = {MAX_FINAL_EDGE_COUNT})\n")
        for key, value in edges_final.items():
            sys.stderr.write(f"For node {key}:{value: 3} >{MAX_FINAL_EDGE_COUNT:3}\n")
        return Codes.DEGREE_FINAL

    if len(edges_med) > 0:
        for key, value in edges_med.items():
            sys.stderr.write(f"For node {key}\n")
            for v in value:
                sys.stderr.write(
                    f"{v[0] / 10 ** 6:.3f} s: {v[1]} > {MAX_MED_EDGE_COUNT}\n"
                )
        return Codes.DEGREE_MED

    return Codes.OK


def clear_graph_files(graph_files, timeout, min_time) -> Codes:
    for i in range(len(graph_files)):
        to_remove = []
        for j in range(len(graph_files[i])):
            name = graph_files[i][j][0]
            real_time = get_time_from_name(name) - min_time
            if real_time > timeout:
                to_remove.append(graph_files[i][j])
        for g in to_remove:
            graph_files[i].remove(g)
        if len(graph_files[i]) == 0:
            sys.stderr.write("One of nodes has no graph files\n")
            return Codes.CRASH
    return Codes.OK


def check_topology_stages(graph_files, mac_to_id, timeout) -> Codes:

    timeout *= 1000 * 1000

    mn_time = get_min_time(graph_files)

    if timeout >= 0:
        res = clear_graph_files(graph_files, timeout, mn_time)
        if res != Codes.OK:
            return res

    # MAKE CSV HERE
    last_csv_components = []
    err_code = make_csv_components(graph_files, last_csv_components)
    if err_code != Codes.OK:
        return err_code

    # CONNECTIVITY CHECK GOES HERE
    last_cc_components = [() for i in range(len(graph_files))]
    connectivity_timestamps = []
    connectivity_ok = check_topology_connectivity(
        graph_files, last_csv_components, connectivity_timestamps, last_cc_components
    )
    print_connectivity_info(mn_time, connectivity_ok, connectivity_timestamps)
    if not connectivity_ok:
        return Codes.TOPOLOGY_CONNECT

    # SYNCHRONIZATION CHECK GOES HERE
    synchronization_timestamps = []
    sync_ok = check_topology_synchronization(
        graph_files, last_cc_components, mac_to_id, synchronization_timestamps
    )
    print_synchronization_info(mn_time, sync_ok, synchronization_timestamps)

    if sync_ok:
        return check_topology_metrics(graph_files, mac_to_id)
    else:
        return Codes.TOPOLOGY_SYNC


def is_dot_file(text):
    return re.match(r"^graph-\d*.dot$", text)


def get_all_graph_files(visulizaton_logs_path):

    devs = [
        d
        for d in listdir(visulizaton_logs_path)
        if isfile(join(visulizaton_logs_path, d)) and is_dot_file(d)
    ]

    devs = sorted(
        devs, key=lambda d: int(d[d.find("-") + 1 : d.find(".")])
    )  # ensure we parse only .dot files

    if len(devs) == 0:
        print("No files found")
        return {}, {}, Codes.FAIL

    mac_to_id = {}

    macs = []
    for d in devs:
        with open(join(visulizaton_logs_path, d)) as fd:
            r = re.findall("..:..:..:..:..:..", fd.readline())
            if len(r) > 0:
                macs.append(r[0])
                mac_to_id[r[0]] = len(macs) - 1

    graph_files = []
    for d in devs:
        graphs = []
        with open(join(visulizaton_logs_path, d)) as fd:
            graphs = parse_graphs(fd, macs)

        if len(graphs) == 0:
            print(
                f"File {join(visulizaton_logs_path, d)} does not have graphs dumped\n"
            )
        graph_files.append(graphs)  # type: ignore

    if min([len(g) for g in graph_files]) == 0:
        print("One or more .dot files do not have graphs dumped\n")
        return {}, {}, Codes.FAIL

    return graph_files, mac_to_id, Codes.OK


def do_check(logs_path, timeout=-1):
    visulizaton_logs_path = f"{logs_path}/visualization"
    if not os.path.isdir(visulizaton_logs_path):
        print(f"Visualization directory {visulizaton_logs_path} does not exist")
        return Codes.FAIL.value

    graph_files, mac_to_id, err_code = get_all_graph_files(visulizaton_logs_path)
    if err_code != Codes.OK:
        return err_code.value
    return check_topology_stages(graph_files, mac_to_id, timeout).value


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog="check_finish.py", description="Check experiment results."
    )
    parser.add_argument(
        "-r",
        "--path-to-results",
        help="Path to directory where results of tests are stored.",
        type=str,
        required=True,
    )

    parser.add_argument(
        "-t",
        "--timeout",
        help="Maximum time in seconds graphs will be processed with. If not provided, all graphs will be processed.",
        type=float,
        required=False,
        default=-1,
    )

    if len(sys.argv) < 2:
        print("Provide correct arguments. See description below:\n")
        print(parser.print_help())
        sys.exit(Codes.FAIL.value)

    args = parser.parse_args()

    result = do_check(args.path_to_results, args.timeout)
    sys.exit(result)
