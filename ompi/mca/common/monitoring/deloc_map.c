/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include "opal/mca/base/mca_base_var.h"
#include "opal/util/argv.h"
#include "orte/util/show_help.h"
#include "orte/util/proc_info.h"
#include "orte/runtime/orte_globals.h"
#include <sys/shm.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <asm-generic/mman-common.h>
#include "deloc_map.h"
#include "map.h"
#include "deloc_metis.h"

int compare_pair(const void * a, const void * b) {
    struct pair *p1 = (struct pair *) a;
    struct pair *p2 = (struct pair *) b;
    return (p2->ncomm - p1->ncomm);
}

void * detectThread(void *args) {
    struct info *f = (struct info *) args;
    pthread_t tid = pthread_self();
    printf("** [DeLoc t#%lu] Starting detector (pid=%d)..\n",
            (unsigned long) tid, f->pid);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    long nproc, i;
    nproc = sysconf(_SC_NPROCESSORS_ONLN);
    while (!stopDelocMon) {
        sleep(5);
        // get current affinity
        const int get_aff = sched_getaffinity(f->pid, sizeof (cpu_set_t), &cpuset);
        printf("** [DeLoc t#%lu rank#%d] Detector wakes up..\n", (unsigned long) tid, f->node_rank);
        printf("*** get_affinity: ");
        for (i = 0; i < nproc; i++) {
            printf("%d ", CPU_ISSET(i, &cpuset));
        }
        printf("\n");
    }
}

void * monitor_exec(void *args) {
    size_t *pml_data = (size_t *) args;
    while (!stopDelocMon) {
        sleep(pollInterval);
        printf("** [rank-%d] pml_count[]= ", pInfo->local_rank);
        for (int i = 0; i < num_local_procs; i++) {
            printf("%d ", (unsigned int) pml_data[i]);
        }
        printf("\n");
        update_commmat_shm(pInfo->shm_name, pml_data, num_local_procs);

        // Get the data from shm
        if (pInfo->local_rank == 0) {
            sleep(1);
            //size_t **shmem_comm = (size_t*) malloc(sizeof(size_t) * nprocs_world);
            //size_t shmem_comm[nprocs_world][nprocs_world];
            /*size_t **shmem_comm = (size_t **)malloc(nprocs_world * sizeof(size_t *)); 
            for (int i = 0; i < nprocs_world; i++) 
                 shmem_comm[i] = (size_t *)malloc(nprocs_world * sizeof(size_t)); 
            
            for (int i = 0 ; i < nprocs_world ; i++) {
                for (int j = 0 ; j < nprocs_world ; j++) {
                    shmem_comm[i][j] = 0;
                }
            }*/
            //get_commmat_shm(pInfo->shm_name, shmem_comm, nprocs_world);
            get_all_commmat_shm();
            printf("** comm_mat[]:\n\t");
            /*for (int i = 0 ; i < nprocs_world ; i++) {
                printf("%d ", (unsigned int) shmem_comm[i]);
            }*/
            for (int i = 0; i < num_local_procs; i++) {
                for (int j = 0; j < num_local_procs; j++) {
                    printf("%d ", (unsigned int) comm_mat[i][j]);
                }
                printf("\n\t");
            }
            printf("\n");

            // Do Mapping
            map_deloc();
            // Enforce the mapping
            get_all_task_shm();
            const char *key;
            map_iter_t iter = map_iter(&proc_pid_maps);

            while ((key = map_next(&proc_pid_maps, &iter))) {
                __pid_t pid = *map_get(&proc_pid_maps, key);
                printf("%s -> %d ", key, pid);
                int rank_id = atoi(key);
                map_proc(pid, task_core[rank_id]);
                get_proc_affinity(pid);
                //map_rank(rank_id, rank_id);
                //cur_mapping[rank_id] = rank_id;
            }
            printf("\n");

            /*
            qsort(pairs, npairs, sizeof (struct pair), compare_pair);
            for (int i = 0; i < npairs; i++) {
                printf("\t%d-%d: %d\n", pairs[i].t1, pairs[i].t2, pairs[i].ncomm);
            }

            //map_proc_rand(pInfo->pid);
            get_all_task_shm();
            const char *key;
            map_iter_t iter = map_iter(&proc_pid_maps);

            while ((key = map_next(&proc_pid_maps, &iter))) {
                __pid_t pid = *map_get(&proc_pid_maps, key);
                printf("%s -> %d ", key, pid);
                get_proc_affinity(pid);
                int rank_id = atoi(key);
                //packed mapping
                map_rank(rank_id, rank_id);
                cur_mapping[rank_id] = rank_id;
            }
            printf("\n");
             */
        }
    }
}

void init_deloc(orte_proc_info_t orte_proc_info, size_t *pml_data) {
    pInfo = (struct info *) malloc(sizeof (struct info));
    get_proc_info(orte_proc_info);
    update_task_shm(pInfo);
    num_local_procs = pInfo->num_local_peers + 1;
    snprintf(pInfo->shm_name, 16, "DELOC-%d", pInfo->local_rank);
    //init_commmat_shm(pInfo->num_peers, pInfo->shm_name);
    //printf("** rank_world=%d, pid=%d\n", rank_world, nprocs_world);
    if (pInfo->local_rank == 0) {
        // Initialize communication matrix
        comm_mat = (size_t **) malloc(num_local_procs * sizeof (size_t *));
        for (int i = 0; i < num_local_procs; i++) {
            comm_mat[i] = (size_t *) malloc(num_local_procs * sizeof (size_t));
        }
        //reset_comm_mat();
        // Get node information
        hwloc_topology_init(&hw_topo);
        hwloc_topology_load(hw_topo);
        num_cores = hwloc_get_nbobjs_by_type(hw_topo, HWLOC_OBJ_CORE);
        num_nodes = hwloc_get_nbobjs_by_type(hw_topo, HWLOC_OBJ_PACKAGE);
        n_cores_per_node = num_cores / num_nodes;
        printf("[DeLoc] Topology num_nodes=%d, num_cores=%d, n_cores_per_node=%d\n", num_nodes,
                num_cores, n_cores_per_node);

        d_tasks = (struct d_task *) malloc(num_local_procs * sizeof (struct d_task));
        npairs = num_local_procs * (num_local_procs - 1) / 2;
        num_tasks = num_local_procs;
        pairs = (struct pair *) malloc(npairs * sizeof (struct pair));
        for (int i = 0; i < npairs; i++) {
            pairs[i].ncomm = 0;
        }
        printf("[DeLoc] Number of process pairs: %d\n", npairs);
        //map_init(&m);
        map_init(&proc_pid_maps);
        cur_mapping = (unsigned *) malloc(num_local_procs * sizeof (unsigned));
        for (int i = 0; i < num_local_procs; i++) {
            cur_mapping[i] = -1;
        }
        // Init numa nodes topology
        node_cpus = (int **) malloc(num_nodes * sizeof (int *));
        for (int i = 0; i < num_nodes; i++) {
            node_cpus[i] = (int *) malloc(n_cores_per_node * sizeof (int));
        }
        // Task to core id mapping with flat and sequential numbering
        task_core = (int *) malloc(num_cores * sizeof (int));
        for (int i = 0; i < num_cores; i++) {
            task_core[i] = -1;
        }

        task_loads = (struct loadObj *) malloc(num_tasks * sizeof (struct loadObj));
        for (int i = 0; i < num_tasks; i++) {
            task_loads[i].id = i;
            task_loads[i].load = 0;
        }
        comm_mat_to_task_loads(comm_mat, task_loads);
        //qsort(task_loads, num_tasks, sizeof (struct loadObj), compare_loadObj_rev);
        for (int i = 0; i < num_tasks; i++) {
            printf("\tTask-%d: %ld\n", task_loads[i].id, task_loads[i].load);
        }
    }
    stopDelocMon = false;
    pollInterval = 5;

    //    for (int i = 0; i < num_local_procs; i++) {
    //        for (int j = i; j < num_local_procs; j++) {
    //            pairs[i]->t1 = i;
    //            pairs[i]->t2 = j;
    //            pairs[i]->ncomm = 0;
    //            pairs[j]->szcomm = 0;
    //        }
    //    }
    pthread_create(&delocThread, NULL, monitor_exec, (void *) pml_data);
}

void reset_comm_mat() {
    for (int i = 0; i < num_local_procs; i++) {
        for (int j = 0; j < num_local_procs; j++) {
            comm_mat[i][j] = 0;
        }
    }
}

void run_detector(__pid_t pid, int rank) {
    struct info *f = (struct info *) malloc(sizeof (struct info));
    f->pid = pid;
    f->node_rank = rank;
    pthread_create(&delocThread, NULL, detectThread, (void *) f);
}

void stop_deloc() {
    stopDelocMon = true;
    del_commmat_shm(pInfo->shm_name);
    if (pInfo->local_rank == 0)
        free(comm_mat);
    free(pInfo);
    free(pairs);
    free(node_cpus);
    free(node_core_start);
    free(task_core);
    printf("[DeLoc-%d] Monitoring stopped\n", pInfo->local_rank);
    //pthread_join(detectorThread, NULL);
    //pthread_exit(NULL);
    //map_deinit(&m);
    map_deinit(&proc_pid_maps);
}

void get_proc_info(orte_proc_info_t orte_proc_info) {
    int id, ret;
    //const char **tmp = NULL;
    const int *tmp = NULL;
    //char **lines = NULL;
    //OMPI_DECLSPEC void update_commmat_shm(const char *shm_name, size_t *data, int np);
    /*
    id = mca_base_var_find("orte", "orte", NULL, "app_rank");
    if (id < 0) {
        orte_show_help("help-orterun.txt", "debugger-mca-param-not-found",
                       true);
        exit(1);
    }

    ret = mca_base_var_get_value (id, &tmp, NULL, NULL);
    if (ret == OPAL_SUCCESS) {
        //lines = opal_argv_split(tmp[0], ':');
        printf("** Proc rank: %d, node_rank=%d, pid=%d\n", tmp[0],
                orte_process_info.my_node_rank, orte_process_info.pid);
        pInfo->rank = orte_process_info.my_node_rank;
        pInfo->pid = orte_process_info.pid;
    }
    orte_process_name_t procName = orte_process_info.my_name;
    orte_proc_t * procObj = orte_get_proc_object(&procName);
    if (procObj != NULL) {
       nodeNumProcs = procObj->node->num_procs; 
    }
    else {
        printf("[WARN] cannot find proc name: %d-%d", procName.jobid,
                procName.vpid);
    }
    int numPeers = orte_process_info.num_local_peers;
     */
    pInfo->node_rank = orte_proc_info.my_node_rank;
    pInfo->local_rank = orte_proc_info.my_local_rank;
    pInfo->pid = orte_proc_info.pid;
    pInfo->num_local_peers = orte_proc_info.num_local_peers;

    printf("[DeLoc-%d] pid=%d, num_peers=%d, local_rank=%d\n", pInfo->node_rank,
            pInfo->pid, pInfo->num_local_peers, pInfo->local_rank);

    // For initial random
    srand(pInfo->pid);
}

void map_proc(__pid_t pid, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    const int set_result = sched_setaffinity(pid, sizeof (cpu_set_t), &cpuset);
    if (set_result != 0) {
        printf("** Err: sched_setaffinity\n");
    } else {
        printf("** Mapped proc #%d to core #%d\n", pid, core_id);
    }
}

void map_rank(unsigned rank_id, int core_id) {
    char key[5];
    sprintf(key, "%d", rank_id);
    __pid_t pid = *map_get(&proc_pid_maps, key);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    const int set_result = sched_setaffinity(pid, sizeof (cpu_set_t), &cpuset);
    if (set_result != 0) {
        printf("** Err: sched_setaffinity\n");
    } else {
        printf("** Mapped proc #%d (pid=%d) to core #%d\n", rank_id, pid, core_id);
    }
}

void get_proc_affinity(__pid_t pid) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    long nproc, i;
    nproc = sysconf(_SC_NPROCESSORS_ONLN);

    const int ret = sched_getaffinity(pid, sizeof (cpu_set_t), &cpuset);
    if (ret == 0) {
        printf("*** CPU affinity: ");
        for (i = 0; i < nproc; i++) {
            printf("%d ", CPU_ISSET(i, &cpuset));
        }
        printf("\n");
    }
}

void map_proc_rand(__pid_t pid) {
    int random_core_id;
    random_core_id = 4 + (rand() % 4);
    map_proc(orte_process_info.pid, random_core_id);
}

void init_commmat_shm(int np, const char *shm_name) {
    const int SIZE = sizeof (size_t) * np;
    //const char* name = "DELOC";
    int shm_fd, ret;
    int* ptr;

    shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    ret = ftruncate(shm_fd, SIZE);
    ptr = mmap(0, SIZE, PROT_WRITE, MAP_SHARED, shm_fd, 0);
    printf("[DeLoc] Created shmem storage for comm: %s\n", shm_name);
    close(shm_fd);
}

void update_commmat_shm(const char *shm_name, size_t *data, int np) {
    const int SIZE = sizeof (size_t) * np;
    int shm_fd, ret;
    size_t *ptr;
    if ((shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666)) >= 0) {
        ret = ftruncate(shm_fd, SIZE);
        ptr = (size_t *) mmap(0, SIZE, PROT_WRITE, MAP_SHARED, shm_fd, 0);
        for (int i = 0; i < np; i++) {
            ptr[i] = data[i];
        }
        munmap(ptr, SIZE);
        close(shm_fd);
    } else {
        printf("Err: failed writing to shm\n");
    }
}

void del_commmat_shm(const char *shm_name) {
    shm_unlink(shm_name);
}

void get_commmat_shm(const char *shm_name, size_t *to_data, int np) {
    const int SIZE = sizeof (size_t) * np;
    int shm_fd;
    size_t *ptr;
    if ((shm_fd = shm_open(shm_name, O_RDONLY, 0666)) >= 0) {
        ptr = (size_t *) mmap(0, SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
        for (int i = 0; i < np; i++) {
            to_data[i] = ptr[i];
        }
        munmap(to_data, SIZE);
        close(shm_fd);
    } else {
        printf("Err: failed reading shm\n");
    }
}

void get_all_commmat_shm() {
    const int SIZE = sizeof (size_t) * num_local_procs;
    char shm_name[16];
    int shm_fd, r, i;
    size_t *ptr;

    reset_comm_mat();
    for (r = 0; r < num_local_procs; r++) {
        snprintf(shm_name, 16, "DELOC-%d", r);
        if ((shm_fd = shm_open(shm_name, O_RDONLY, 0666)) >= 0) {
            ptr = (size_t *) mmap(0, SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
            for (i = 0; i < num_local_procs; i++) {
                comm_mat[r][i] += ptr[i];
                comm_mat[i][r] += ptr[i];
            }
            //munmap(comm_mat, SIZE);
            close(shm_fd);
        } else {
            printf("Error: failed reading shm\n");
        }
    }
    comm_mat_to_pairs(comm_mat, pairs);
}

void get_all_task_shm() {
    const int SIZE = sizeof (size_t) * num_local_procs;
    char shm_name[24];
    int shm_fd, r;
    struct info *ptr;

    __pid_t *val = map_get(&proc_pid_maps, "0");
    if (val == NULL) {
        for (r = 0; r < num_local_procs; r++) {
            snprintf(shm_name, 24, "DELOC-info-%d", r);
            if ((shm_fd = shm_open(shm_name, O_RDONLY, 0666)) >= 0) {
                ptr = (struct info *) mmap(0, SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
                char rank_s[16];
                snprintf(rank_s, 16, "%d", ptr->local_rank);

                map_set(&proc_pid_maps, rank_s, ptr->pid);
                d_tasks[r].task_id = ptr->pid;

                close(shm_fd);
            } else {
                printf("Error: failed reading tasks shm\n");
            }
        }
    }
}

void update_task_shm(struct info *p_info) {
    char info_shm[24];
    snprintf(info_shm, 16, "DELOC-info-%d", pInfo->local_rank);
    const int SIZE = sizeof (struct info);
    int shm_fd, ret;
    struct info *ptr;
    if ((shm_fd = shm_open(info_shm, O_CREAT | O_RDWR, 0666)) >= 0) {
        ret = ftruncate(shm_fd, SIZE);
        ptr = (struct info *) mmap(0, SIZE, PROT_WRITE, MAP_SHARED, shm_fd, 0);
        ptr->local_rank = p_info->local_rank;
        ptr->node_rank = p_info->node_rank;
        ptr->num_local_peers = p_info->num_local_peers;
        ptr->pid = p_info->pid;
        ptr->ts = p_info->ts;

        munmap(ptr, SIZE);
        close(shm_fd);
    } else {
        printf("Err: failed writing task info to shm\n");
    }
}

void comm_mat_to_pairs(size_t **mat, struct pair *pairs) {
    int j = 0;
    for (int r = 0; r < num_local_procs; r++) {
        for (int i = r + 1; i < num_local_procs; i++) {
            pairs[j].t1 = r;
            pairs[j].t2 = i;
            pairs[j].ncomm = mat[r][i];
            j++;
        }
    }
}

// DeLocMap implementation starts here

int compare_task(const void *a, const void *b) {
    const size_t *ia = (const size_t *) a; // casting pointer types 
    const size_t *ib = (const size_t *) b;
    return *ib - *ia;
}

int compare_loadObj(const void *a, const void *b) {
    const struct loadObj *ia = (const struct loadObj *) a; // casting pointer types 
    const struct loadObj *ib = (const struct loadObj *) b;
    return ia->load - ib->load;
}

int compare_loadObj_rev(const void *a, const void *b) {
    const struct loadObj *ia = (const struct loadObj *) a; // casting pointer types 
    const struct loadObj *ib = (const struct loadObj *) b;
    return ib->load - ia->load;
}

void comm_mat_to_task_loads(size_t **mat, struct loadObj *task_loads) {
    int t = 0;
    for (int i = 0; i < num_tasks; i++) {
        task_loads[t].id = i;
        for (int j = 0; j < num_tasks; j++) {
            task_loads[t].load += mat[i][j];
        }
        t++;
    }
}

bool is_avail(int node_id) {
    return node_core_start[node_id] < n_cores_per_node;
}

int free_cpu(int node_id) {
    return n_cores_per_node - (node_core_start[node_id] - 1);
}

int next_node(int node_id) {
    return ((node_id + 1) % num_nodes);
}

void reset_node_core_start() {
    for (int i = 0; i < num_nodes; i++) {
        node_core_start[i] = 0;
    }
}

void print_node_cpus() {
    for (int i = 0; i < num_nodes; i++) {
        for (int j = 0; j < n_cores_per_node; j++) {
            printf("Node-%d, core-%d -> task-%d\n", i, j, node_cpus[i][j]);
        }
    }
}

int ser_core_to_node(int core_id) {
    printf("** ser_core: %d\n", core_id);
    int div = core_id / n_cores_per_node;
    /*
    int mod = core_id % n_cores_per_node;
    if (mod > 0) {
        div++;
    }*/
    return (div);
}

int node_core_to_ser(int node_id, int core_id) {
    return (node_id * n_cores_per_node) +core_id;
}

int map_to_next_core(int node_id, int task_id) {
    int target_node = -1;
    int target_core = -1;
    if (is_avail(node_id)) {
        target_node = node_id;
        target_core = node_core_start[target_node];
        node_cpus[node_id][target_core] = task_id;
        node_core_start[target_node]++;
    } else {
        int tried = 0;
        int cand_node = next_node(node_id);
        while ((target_node == -1) && (tried < num_nodes)) {
            if (is_avail(cand_node)) {
                target_node = cand_node;
                target_core = node_core_start[target_node];
                node_cpus[target_node][target_node] = task_id;
                node_core_start[target_node]++;
            }
            tried++;
            cand_node = next_node(cand_node);
        }
    }
    if (target_node > -1) {
        task_core[task_id] = node_core_to_ser(target_node, target_core);
    }
    return target_node;
}

void map_deloc() {
    int i, n_mapped, mult;
    node_core_start = (int *) malloc(num_nodes * sizeof (int));
    for (i = 0; i < num_nodes; i++) {
        node_core_start[i] = 0;
    }
    bool is_mapped[num_tasks];
    for (i = 0; i < num_tasks; i++) {
        is_mapped[i] = false;
    }

    printf("* Running DeLoc mapping.. \n");
    n_mapped = 0;
    qsort(pairs, npairs, sizeof (struct pair), compare_pair);
    //mult = 0;
    i = 0;
    //int curr_node = 0;
    while (n_mapped < num_tasks && i < npairs) {
        struct pair p = pairs[i];
        //int curr_node = mult % num_nodes;
        //int t1_node = curr_node;
        int t1_node = i % num_nodes;
        printf("** curr_node=%d, curr_pair: %d-%d (%ld)\n", t1_node, p.t1,
                p.t2, p.ncomm);

        if (!is_mapped[p.t1]) {
            t1_node = map_to_next_core(t1_node, p.t1);
            if (t1_node > -1) {
                n_mapped++;
                is_mapped[p.t1] = true;
            }
        } else {
            // Mapped before
            t1_node = ser_core_to_node(task_core[p.t1]);
            printf("T1 mapped before to: %d\n", t1_node);
        }
        if (!is_mapped[p.t2]) {
            t1_node = map_to_next_core(t1_node, p.t2);
            if (t1_node > -1) {
                n_mapped++;
                is_mapped[p.t2] = true;
            }
        }

        //mult++;
        i++;
    }
    print_node_cpus();
}

void map_deloc_tl() {
    int t, n;
    /*
    node_core_start = (int *) malloc(num_nodes * sizeof (int));
    for (n = 0; n < num_nodes; n++) {
        node_core_start[n] = 0;
    }*/
    reset_node_core_start();

    // Reset node loads
    node_loads = (struct loadObj*) malloc(num_nodes * sizeof (struct loadObj));
    for (n = 0; n < num_nodes; n++) {
        node_loads[n].id = n;
        node_loads[n].load = 0;
    }

    printf("* Running DeLocTL mapping.. \n");
    t = 0;
    //n = 0;
    //qsort(task_loads, num_tasks, sizeof (struct loadObj), compare_loadObj_rev);
    while (t < num_tasks) {
        n = node_loads[0].id;
        int target_node = map_to_next_core(n, task_loads[t].id);
        printf("** Least node=%d, Task-%d->Node-%d\n", n, task_loads[t].id,
                target_node);
        node_loads[0].load += task_loads[t].load;
        qsort(node_loads, num_nodes, sizeof (struct loadObj), compare_loadObj);
        //        for (int i = 0; i < num_nodes; i++) {
        //            printf("** Node[%d]=%ld\n", node_loads[i].id, node_loads[i].load);
        //        }
        t++;
    }

    print_node_cpus();
}
