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


int compare_pair (const void * a, const void * b) {
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
            
            qsort(pairs, npairs, sizeof(struct pair), compare_pair);
            for (int i = 0; i < npairs; i++) {
                printf("\t%d-%d: %d\n", pairs[i].t1, pairs[i].t2, pairs[i].ncomm);
            }
            
            //map_proc_rand(pInfo->pid);
        }
    }
}

void init_deloc(orte_proc_info_t orte_proc_info, size_t *pml_data) {
    pInfo = (struct info *) malloc(sizeof (struct info));
    get_proc_info(orte_proc_info);
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
        printf("[DeLoc] Topology num_nodes=%d, num_cores=%d\n", num_nodes,
                num_cores);
        
        npairs = num_local_procs * (num_local_procs-1) /2;
        pairs = (struct pair *) malloc(npairs * sizeof (struct pair));
        for (int i = 0; i < npairs; i++) {
            pairs[i].ncomm = 0;
        }
        printf("[DeLoc] Number of process pairs: %d\n", npairs);
	map_init(&m);
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
    printf("[DeLoc-%d] Monitoring stopped\n", pInfo->local_rank);
    //pthread_join(detectorThread, NULL);
    //pthread_exit(NULL);
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

void comm_mat_to_pairs(size_t **mat, struct pair *pairs) {
    int j = 0;
    for (int r = 0; r < num_local_procs; r++) {
        for (int i = r+1; i < num_local_procs; i++) {
            pairs[j].t1 = r;
            pairs[j].t2 = i;
            pairs[j].ncomm = mat[r][i];
            j++;
        }
    }
}