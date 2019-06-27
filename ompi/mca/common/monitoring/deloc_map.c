/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
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
#include <numa.h>
#include <numaif.h>
#include <time.h>
#include "deloc_map.h"
#include "map.h"
#include "deloc_metis.h"
#define DELOC_MASTER        0
#define DELOC_COMM_FILE     "deloc.comm.csv"
#define MASTER_DELAY        250000
#define DELAY_STEP          500000
#define DELAY_STEP_MAX      1000000
#define INTERVAL_MAX_DEF    10000000
#define INTERVAL_MIN        500000
#define SIZE_POLL_ITV       sizeof(unsigned long)
#define SIZE_PID            sizeof(__pid_t)
#define MAX_RECORD_ITV      24
#define SHM_POLL_ITV        "DELOC-pollItv"

int compare_pair(const void * a, const void * b) {
    struct pair *p1 = (struct pair *) a;
    struct pair *p2 = (struct pair *) b;
    return (p2->ncomm - p1->ncomm);
}

void * monitor_exec_measure(void *args) {
    int i, n_poll, mat_size, diff;//, diff2;//, j;
    struct timespec start, end;
    size_t *pml_data = (size_t *) args;
    mat_size = sizeof (size_t) * num_local_procs;
    int *itvs = (int *) malloc(MAX_RECORD_ITV * sizeof (int));

    n_poll = 0;
    is_thread_running = true;
    delay_poll(pollInterval); // First wait
    //usleep(pollInterval); // First wait
    while (!stopDelocMon && n_poll < pollNMax) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        //update_commmat_shm(pInfo->shm_name, prev_pml_data, num_local_procs);
        //update_commmat_shm(pInfo->shm_name, pml_data, num_local_procs);
        update_commmat_shm(shm_name_mat, pml_data, num_local_procs);
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        poll_time_used += (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000.0;

        // Get the data from shm
        //if (pInfo->local_rank == DELOC_MASTER) {
        if (proc_info.my_local_rank == DELOC_MASTER) {
            // Delay to let all of the task finish updating
            //usleep(150000);
            usleep(MASTER_DELAY);
            
            clock_gettime(CLOCK_MONOTONIC, &start);
            // Update comm. matrix
            get_all_commmat_shm(mat_size);

            // Do Mapping
              
            //comm_mat_to_pairs(comm_mat, pairs);
            comm_mat_to_pairs_tasks(comm_mat, pairs, task_loads);
            qsort(pairs, npairs, sizeof (struct pair), compare_pair);
            //printf("Compare update task most..\n");
            diff = compare_update_task_most(task_loads, task_loads_prev, num_tasks, n_comm_changed);
            //diff = compare_update_pairs(pairs, pairs_prev, npairs_prev);
            //diff2 = compare_update_task_most(task_loads, task_loads_prev, num_tasks);
            //print_pairs(pairs, npairs);
            
            /* for deloc_tl and balance */
            /* 
            comm_mat_to_task_loads(comm_mat, task_loads);
            qsort(task_loads, num_tasks, sizeof (struct loadObj), compare_loadObj_rev);
            diff = compare_update_task_loads(task_loads, task_loads_prev, num_tasks);
            print_task_loads();
            */
            
            // Skip mapping if the comm pattern does not change,
            // make interval longer
            //printf("[DeLoc] diff1=%d, diff2=%d\n", diff, diff2);
            //if (diff < n_cores_per_node) {
            if (diff < tasks_diff_lim){
                if (pollInterval < pollIntervalMax){
                    pollInterval *= slope_m;
                }
            }
            else {
                if (pollInterval > INTERVAL_MIN) {
                    pollInterval *= slope_n;
                }
                map_deloc_if();
                //map_deloc();
                //map_deloc_tl();
                //map_balance();
                //map_locality();
            
                // Get the PIDs
                if (num_pids < num_tasks) {
                    //get_all_task_shm();
                    get_all_pids_shm();
                }
                
                // Enforce the mapping
                // Array based implementation
                for (i = 0; i < num_tasks; i++) {
                    //printf("Task-%d: pid #%d ", i, task_pids[i]);
                    if (task_core[i] != task_core_prev[i]) {
                        map_proc(task_pids[i], task_core[i]);
                    }
                    //get_proc_affinity(task_pids[i]);
                }
                // Save current map to prev
                update_task_core_prev();
            
                clock_gettime(CLOCK_MONOTONIC, &end);
                mapping_time_used += (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000.0;
                n_comm_changed++;
                save_comm_mat_part(num_tasks, n_poll);
            }
        }
        if (n_poll < MAX_RECORD_ITV) {
            itvs[n_poll] = pollInterval;
        }
        n_poll++;
        delay_poll(pollInterval);
        //usleep(pollInterval);
    }
    is_thread_running = false;
    //if (pInfo->local_rank == DELOC_MASTER) {
    if (proc_info.my_local_rank == DELOC_MASTER) {
        printf("[DeLoc] Mapping intervals:");
        for (i = 0; i < n_poll; i++) {
            printf(" %d", itvs[i]);
        }
        putchar('\n');
    }
    pthread_exit(NULL);
}

void delay_poll(unsigned long itv) {
    unsigned int step;
    unsigned long i = itv;
    if (i >= DELAY_STEP_MAX)
        step = DELAY_STEP_MAX;
    else
        step = DELAY_STEP;
    while (i >= step && !stopDelocMon) {
        usleep(step);
        i -= step;
    }
    if (i > 0 && !stopDelocMon) {
        usleep(i);
    }
}

void * monitor_exec(void *args) {
    int i, n_poll, mat_size, diff;//, diff2;//, j;
    //struct timespec req = {0};
    size_t *pml_data = (size_t *) args;
    mat_size = sizeof (size_t) * num_local_procs;

    n_poll = 0;
    is_thread_running = true;
    delay_poll(pollInterval); // First wait
    while (!stopDelocMon && n_poll < pollNMax) {
        //usleep(pollInterval);
       // pollInterval *= 2;
        /*
        printf("** [rank-%d] pml_count[]= ", pInfo->local_rank);
        for (int i = 0; i < num_local_procs; i++) {
            printf("%d ", (unsigned int) pml_data[i]);
        }
        printf("\n");
        */
        //
        //for (i = 0; i < num_local_procs; i++) {
        //    prev_pml_data[i] = pml_data[i] - prev_pml_data[i]; 
        //}
        //start = clock();
        //clock_gettime(CLOCK_MONOTONIC, &start);
        //update_commmat_shm(pInfo->shm_name, prev_pml_data, num_local_procs);
        //update_commmat_shm(pInfo->shm_name, pml_data, num_local_procs);
        update_commmat_shm(shm_name_mat, pml_data, num_local_procs);
        //clock_gettime(CLOCK_MONOTONIC, &end);
        //poll_time_used += (double) ((clock()-start) / (CLOCKS_PER_SEC/1000));
        //poll_time_used += (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000.0;

        // Get the data from shm
        //if (pInfo->local_rank == DELOC_MASTER) {
        if (proc_info.my_local_rank == DELOC_MASTER) {
            // Delay to let all of the task finish updating
            //usleep(150000);
            //usleep(1000000);
            usleep(MASTER_DELAY);
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
            
            //clock_gettime(CLOCK_MONOTONIC, &start);
            // Update comm. matrix
            //get_commmat_shm(pInfo->shm_name, shmem_comm, nprocs_world);
            //printf("Get all commat from shm..\n");
            get_all_commmat_shm(mat_size);
            /*for (int i = 0 ; i < nprocs_world ; i++) {
                printf("%d ", (unsigned int) shmem_comm[i]);
            }*/
            /*
            printf("** Communication matrix:\n\t");
            for (i = 0; i < num_local_procs; i++) {
                for (j = 0; j < num_local_procs; j++) {
                    printf("%d ", (unsigned int) comm_mat[i][j]);
                }
                printf("\n\t");
            }
            putchar('\n');
            */
            // Update task loads, required by DeLocTL
            
            //comm_mat_to_task_loads(comm_mat, task_loads);
            /*
            for (int i = 0; i < num_tasks; i++) {
                printf("\tTask-%d, load=%ld\n", task_loads[i].id, task_loads[i].load);
            }
            */

            // Do Mapping
              
            //comm_mat_to_pairs(comm_mat, pairs);
            comm_mat_to_pairs_tasks(comm_mat, pairs, task_loads);
            qsort(pairs, npairs, sizeof (struct pair), compare_pair);
            //diff = compare_update_pairs(pairs, pairs_prev, npairs_prev);
            //printf("Compare update task most..\n");
            diff = compare_update_task_most(task_loads, task_loads_prev, num_tasks, n_comm_changed);
            //print_pairs(pairs, npairs);
            
            // Update previous pairs
            /*
            for (int i=0; i < npairs_prev; i++) {
                pairs_prev[i].t1 = pairs[i].t1;
                pairs_prev[i].t2 = pairs[i].t2;
                pairs_prev[i].ncomm = pairs[i].ncomm;
            }*/
            /*
            if (diff >= n_cores_per_node) {
                printf("** Comm. change, do mapping\n");
            } else {
                printf("** Comm. does not change, skipped mapping\n");
                pollInterval *= 2;
                continue;
            }*/
            // Skip mapping if the comm pattern does not change,
            // make interval longer
            /* for deloc_tl and balance */
            /* 
            comm_mat_to_task_loads(comm_mat, task_loads);
            qsort(task_loads, num_tasks, sizeof (struct loadObj), compare_loadObj_rev);
            diff = compare_update_task_loads(task_loads, task_loads_prev, num_tasks);
            print_task_loads();
            */
            //printf("[DeLoc] diff1=%d, diff2=%d\n", diff, diff2);
            //printf("[DeLoc] diff=%d, lim=%d\n", diff, diff_lim);
            //if (diff < n_cores_per_node) {
            //if (diff < tasks_diff_lim) {
            if (diff < tasks_diff_lim) {
                //printf("** Comm. does not change, skipped mapping\n");
                //print_pairs(pairs,npairs);
                //print_pairs(pairs_prev,npairs_prev);

                // Slowage the polling
                //printf("\tCurrent PollInterval: %d us\n", pollInterval);
                if (pollInterval < pollIntervalMax){
                    pollInterval *= slope_m;
                }
                //pollInterval *= slope_m;
                //continue;
            }
            else {
                if (pollInterval > INTERVAL_MIN) {
                    pollInterval *= slope_n;
                }
                map_deloc_if();
                //map_deloc();
                //map_deloc_tl();
                //map_balance();
                //map_locality();
            
                // Get the PIDs
                if (num_pids < num_tasks) {
                    //get_all_task_shm();
                    get_all_pids_shm();
                }
            
                // Enforce the mapping
                // Array based implementation
                for (i = 0; i < num_tasks; i++) {
                    //printf("Task-%d: pid #%d ", i, task_pids[i]);
                    if (task_core[i] != task_core_prev[i]) {
                    //    && numa_cores_node[task_core[i]] != numa_cores_node[task_core_prev[i]]) {
                         map_proc(task_pids[i], task_core[i]);
                    }
                    //get_proc_affinity(task_pids[i]);
                }
                
                // Save current map to prev
                update_task_core_prev();
            
            //putchar('\n');
            
            /* KeyMap implementation
            const char *key;
            map_iter_t iter = map_iter(&proc_pid_maps);

            while ((key = map_next(&proc_pid_maps, &iter))) {
                __pid_t pid = *map_get(&proc_pid_maps, key);
                printf("Task-%s: pid #%d ", key, pid);
                int rank_id = atoi(key);
                //map_proc(pid, task_core[rank_id]);
                // Use the physical id for binding
                map_proc(pid, task_core[rank_id]);
                get_proc_affinity(pid);
                //map_rank(rank_id, rank_id);
                //cur_mapping[rank_id] = rank_id;
            }
            printf("\n");
            */
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
            //clock_gettime(CLOCK_MONOTONIC, &end);
            //mapping_time_used += (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000.0;
                n_comm_changed++;
            }
            update_poll_itv_shm(pollInterval);
        }
        n_poll++;
        //usleep(pollInterval);
        //usleep(pollInterval); // First wait
        get_poll_itv_shm();
        //printf("\tCurrent PollInterval: %lu us\n", pollInterval);
        delay_poll(pollInterval);
        //req.tv_sec = 0;
        //req.tv_nsec = pollInterval * 1000000L;
        //nanosleep(&req, (struct timespec *) NULL); // First wait
    }
    is_thread_running = false;
    //free_mem_master();
    pthread_exit(NULL);
    //return NULL;
}

void init_deloc(orte_proc_info_t orte_proc_info, size_t *pml_data) {
    int i, j;

    const char* env_deloc_enable = getenv("DELOC_ENABLED");
    if (env_deloc_enable != NULL && atoi(env_deloc_enable) == 0) {
        deloc_enabled = 0;
        return; 
    }
    
    deloc_enabled = 1;

    //pInfo = (struct info *) malloc(sizeof (struct info));
    //get_proc_info(orte_proc_info);
    //update_task_shm(pInfo);
    //num_local_procs = pInfo->num_local_peers + 1;
    //snprintf(pInfo->shm_name, 16, "DELOC-%d", pInfo->local_rank);
    proc_info = orte_proc_info;
    num_local_procs = proc_info.num_local_peers + 1;
    // Set names for shm
    snprintf(shm_name_mat, SHM_N_LEN, "DELOC-%d", proc_info.my_local_rank);
    snprintf(shm_name_pid, SHM_N_LEN, "DELOC-p-%d", proc_info.my_local_rank);
    // Update this process id to shm
    update_my_pid_shm();

    // Set Polling interval (in us)    
    const char* env_poll_itv = getenv("DELOC_POLL_INTERVAL");
    if (env_poll_itv != NULL) {
        pollInterval = atoi(env_poll_itv);
        //printf("[DeLoc] Found poll interval envs: %d\n", pollInterval);
    }
    else {
        // Default interval is 2 s
        pollInterval = 2000000;
        //pollInterval = 2000;
    }
    const char* env_poll_max = getenv("DELOC_POLL_MAX");
    if (env_poll_max != NULL) {
        pollNMax = atoi(env_poll_max);
    }
    else {
        pollNMax = 9999;
    }
    
    // Initialize delta pml data
    //prev_pml_data = (size_t *) malloc(num_local_procs * sizeof (size_t));
    //for (i = 0; i < num_local_procs; i++) {
    //    prev_pml_data[i] = 0;
    //}
    //printf("** rank_world=%d, pid=%d\n", rank_world, nprocs_world);
    //if (pInfo->local_rank == DELOC_MASTER) {
    if (proc_info.my_local_rank == DELOC_MASTER) {
        // For rand()
        //srand(pInfo->pid);
        //printf("[DeLoc] Application name: %s\n",  ORTE_APP_CONTEXT->app);
        update_poll_itv_shm(pollInterval);

        // Load settings from the environment
        const char* env_silent_mode = getenv("DELOC_SILENT_MODE");
        if (env_silent_mode != NULL && atoi(env_silent_mode) == 1) {
            silent_mode = true;
        }
        else {
            silent_mode = false;
        }

        const char* env_itv_max = getenv("DELOC_POLL_INTERVAL_MAX");
        if (env_itv_max != NULL) {
            pollIntervalMax = atoi(env_itv_max);
        }
        else {
            pollIntervalMax = INTERVAL_MAX_DEF;
        }
        //printf("[DeLoc] Poll interval: %lu (max=%lu) microsecs, max_count: %d\n", pollInterval,
        //         pollIntervalMax, pollNMax);
    
        const char* env_pairs_equal = getenv("DELOC_PAIRS_EQ_RATIO");
        if (env_pairs_equal != NULL) {
            pairs_equal_ratio = atof(env_pairs_equal);   
        }
        else {
            pairs_equal_ratio = 0.5;
        }
        const char* env_slope_m = getenv("DELOC_SLOPE_M");
        const char* env_slope_n = getenv("DELOC_SLOPE_N");
        if (env_slope_m != NULL && env_slope_n != NULL) {
            slope_m = atof(env_slope_m);
            slope_n = atof(env_slope_n);
        }
        else {
            //slope_m = slope_n = 2;
            //slope_m = slope_n = 1.5;
            slope_m = 1.5;
            slope_n = 0.7;
        }
    
        const char* env_export_mat = getenv("DELOC_EXPORT_MAT");
        if (env_export_mat != NULL && atoi(env_export_mat) == 1) {
            export_comm_mat = true;
        }
        else {
            export_comm_mat = false;
        }

        // Initialize communication matrix
        comm_mat = (size_t **) malloc(num_local_procs * sizeof (size_t *));
        //prev_comm_mat = (size_t **) malloc(num_local_procs * sizeof (size_t *));
        for (i = 0; i < num_local_procs; i++) {
            comm_mat[i] = (size_t *) malloc(num_local_procs * sizeof (size_t));
            //prev_comm_mat[i] = (size_t *) malloc(num_local_procs * sizeof (size_t));
            for (j = 0; j < num_local_procs; j++) {
                comm_mat[i][j] = 0;
            }
        }
        /*
        for (i = 0; i < num_local_procs; i++) {
            for (j = 0; j < num_local_procs; j++) {
        //        prev_comm_mat[i][j] = 0;
                comm_mat[i][j] = 0;
            }
        }*/
        //reset_comm_mat();
        // Get node information
        hwloc_topology_init(&hw_topo);
        hwloc_topology_load(hw_topo);
        num_cores = hwloc_get_nbobjs_by_type(hw_topo, HWLOC_OBJ_CORE);
        int num_pus = hwloc_get_nbobjs_by_type(hw_topo, HWLOC_OBJ_PU);
        //num_nodes = hwloc_get_nbobjs_by_type(hw_topo, HWLOC_OBJ_PACKAGE);
        //num_nodes = hwloc_get_nbobjs_by_type(hw_topo, HWLOC_OBJ_NUMANODE);
        // In some hws such as KNL, the processor node is shown by the GROUP object
        num_nodes = hwloc_get_nbobjs_by_type(hw_topo, HWLOC_OBJ_GROUP);
        if (num_nodes <= 0) {
            num_nodes = hwloc_get_nbobjs_by_type(hw_topo, HWLOC_OBJ_SOCKET);
        }
        //printf("[DeLoc] Topology num_nodes=%d, num_pus=%d, num_cores=%d, n_pus_per_node=%d\n", num_nodes,
        //        num_pus, num_cores, num_pus/num_nodes);

        //d_tasks = (struct d_task *) malloc(num_local_procs * sizeof (struct d_task));
        num_tasks = num_local_procs;
        npairs = num_tasks * (num_tasks - 1) / 2;
        pairs = (struct pair *) malloc(npairs * sizeof (struct pair));
        for (i = 0; i < npairs; i++) {
            pairs[i].ncomm = 0;
        }
        //printf("[DeLoc] Number of tasks=%d, number of pairs=%d\n", num_tasks, npairs);

        //npairs_prev = num_tasks * pairs_equal_ratio;
        npairs_prev = num_tasks * 1.4;  // diagonal
        diff_lim = npairs_prev * pairs_equal_ratio;
        pairs_prev = (struct pair *) malloc(npairs_prev * sizeof (struct pair));
        ntasks_prev = num_tasks * 0.5;
        tasks_diff_lim = ntasks_prev * (1-pairs_equal_ratio);
        ntasks_cmp = ntasks_prev * 2;   // doubled for the comparison step

        // Use physical core if possible
        if (num_tasks > num_cores && num_tasks <= num_pus) {
            num_cores = num_tasks;
        }

        n_cores_per_node = num_cores / num_nodes;
        if (num_cores % num_nodes > 0) {
            n_cores_per_node++;
            num_cores = n_cores_per_node * 2;
        }

        num_pids = 0;
        task_pids = (__pid_t *) malloc(num_tasks * sizeof(__pid_t));
        is_mapped = (bool *) malloc(num_tasks * sizeof (bool));
        //map_init(&proc_pid_maps);
        /*
        cur_mapping = (unsigned *) malloc(num_local_procs * sizeof (unsigned));
        for (i = 0; i < num_local_procs; i++) {
            cur_mapping[i] = -1;
        }
        */
        // Init numa nodes topology
        //node_cpus = (int **) malloc(num_nodes * sizeof (int *));
        //for (i = 0; i < num_nodes; i++) {
        //    node_cpus[i] = (int *) malloc(n_cores_per_node * sizeof (int));
        //}
        // Print node CPU using numactl
        //int num_numa_cores = numa_num_configured_cpus();
        numa_cores = (int *) malloc (num_cores * sizeof(int));
        numa_cores_node = (int *) malloc (num_cores * sizeof(int));
        get_numa_cpus();
        /*
        printf("[NUMACTL] (Node IDs):(Physical core IDs)\n");
        for (i = 0; i < num_cores; i++) {
            printf(" %d:%d", numa_cores_node[i], numa_cores[i]);
        }
        putchar('\n');
        */
        /*
        for (int i = 0; i < num_nodes; i++) {
            print_numa_node_cpus(i);
        }
        */
        // Task to core id mapping with flat and sequential numbering
        task_core = (int *) malloc(num_tasks * sizeof (int));
        task_core_prev = (int *) malloc(num_tasks * sizeof (int));
        for (i = 0; i < num_tasks; i++) {
            //task_core[i] = task_core_prev[i] = -1;
            task_core[i] = task_core_prev[i] = i;
        }
        node_core_start = (int *) malloc(num_nodes * sizeof (int));

        // DeLocTL and Balance use the datas below
        task_loads = (struct loadObj *) malloc(num_tasks * sizeof (struct loadObj));
        task_loads_prev = (struct loadObj *) malloc(ntasks_prev * sizeof (struct loadObj));
        task_loads_cmp = (struct loadObj *) malloc(ntasks_cmp * sizeof (struct loadObj));
         
        for (int i = 0; i < ntasks_prev; i++) {
            task_loads_prev[i].id = i;
            task_loads_prev[i].load = 0;
        }
        node_loads = (struct loadObj*) malloc(num_nodes * sizeof (struct loadObj));
        for (i = 0; i < num_nodes; i++) {
            node_loads[i].id = i;
            //node_loads[n].load = 0;
        }

        n_comm_changed = 0;
        mapping_time_used = 0;

        /*
        comm_mat_to_task_loads(comm_mat, task_loads);
        //qsort(task_loads, num_tasks, sizeof (struct loadObj), compare_loadObj_rev);
        for (int i = 0; i < num_tasks; i++) {
            printf("\tTask-%d, load=%ld\n", task_loads[i].id, task_loads[i].load);
        }
        */
        // Print information
        if (silent_mode == false) {
            //printf("/** On-DeLoc Information **/\n"
            //       "\tNumber of nodes: %d\n"
            printf("[DeLoc] Poll interval: %lu (max=%lu) microsecs, max_count: %d\n", pollInterval,
                     pollIntervalMax, pollNMax);
            printf("[DeLoc] Topology num_nodes=%d, num_pus=%d, num_cores=%d, n_pus_per_node=%d\n",
                    num_nodes, num_pus, num_cores, num_pus/num_nodes);
            printf("[DeLoc] Number of tasks=%d, number of pairs=%d\n", num_tasks, npairs);
    
            printf("[NUMACTL] (Node IDs):(Physical core IDs)\n");
            for (i = 0; i < num_cores; i++) {
                printf(" %d:%d", numa_cores_node[i], numa_cores[i]);
            }
            putchar('\n');
        }

        // Perform initial mapping
        usleep(MASTER_DELAY);   // Delay to let all processes finish updating their pid
        //get_all_task_shm();
        get_all_pids_shm();
        for (i = 0; i < num_tasks; i++) {
            map_proc(task_pids[i], task_core[i]);
        }
    }
    stopDelocMon = false;
    poll_time_used = 0; 


    //    for (int i = 0; i < num_local_procs; i++) {
    //        for (int j = i; j < num_local_procs; j++) {
    //            pairs[i]->t1 = i;
    //            pairs[i]->t2 = j;
    //            pairs[i]->ncomm = 0;
    //            pairs[j]->szcomm = 0;
    //        }
    //    }
    //pml_events = pml_data;
    
    const char* env_measure = getenv("DELOC_MEASURE_TIME");
    if (env_measure != NULL && atoi(env_measure) > 0) {
        time_measured = 1;
        pthread_create(&delocThread, NULL, monitor_exec_measure, (void *) pml_data);
    }
    else {
        time_measured = 0;
        pthread_create(&delocThread, NULL, monitor_exec, (void *) pml_data);
    }
}

void reset_comm_mat() {
    for (int i = 0; i < num_local_procs; i++) {
        for (int j = 0; j < num_local_procs; j++) {
            comm_mat[i][j] = 0;
            //prev_comm_mat[i][j] = 0;
        }
    }
}

void reset_prev_comm_mat() {
    for (int i = 0; i < num_local_procs; i++) {
        for (int j = 0; j < num_local_procs; j++) {
            prev_comm_mat[i][j] = 0;
        }
    }
}

void stop_deloc() {
    //char info_shm[16];
    if (deloc_enabled == 1) {
        stopDelocMon = true;
        // Update the last state of comm. matrix
        //update_commmat_shm(pInfo->shm_name, pml_events, num_local_procs);
        
        while (is_thread_running == true) {
            // wait for main thread finished
            //sleep(1);
            usleep(100000); //100 ms
        }
        //del_shm(pInfo->shm_name);
        //snprintf(info_shm, 16, "DELOC-t-%d", pInfo->local_rank);
        //del_shm(info_shm);
        //snprintf(info_shm, 16, "DELOC-p-%d", proc_info.my_local_rank);
        //del_shm(shm_name_pid);
        //del_shm(shm_name_mat);

        if (time_measured == 1) {
            //printf("[DeLoc-%d],elapsed_time,update_comm(),%f,ms\n", pInfo->local_rank, poll_time_used);
            printf("[DeLoc-%d],elapsed_time,update_comm(),%f,ms\n",
                        proc_info.my_local_rank, poll_time_used);
        }

         //printf("[DeLoc-%d] Cleaning up..\n", pInfo->local_rank);
        //free(prev_pml_data);
        //if (pInfo->local_rank == DELOC_MASTER) {
        if (proc_info.my_local_rank == DELOC_MASTER) {
            if (time_measured == 1) {
                printf("[DeLoc-%d],elapsed_time,map(),%f,ms\n", DELOC_MASTER, mapping_time_used);
            }
            // Save the last state of comm. matrix
            //get_all_commmat_shm(sizeof (size_t) * num_local_procs);
            //print_comm_mat(comm_mat);
            if (export_comm_mat == true) {
                save_comm_mat(num_tasks);
            }

            // Always print this information
            printf("[DeLoc] Mapping was applied %d time(s).\n", n_comm_changed);
            // Cleanup
            del_shm(SHM_POLL_ITV);
            free_mem_master();
            //free(prev_comm_mat);
        }
        del_shm(shm_name_pid);
        del_shm(shm_name_mat);

        //free(pInfo);
        //printf("[DeLoc-%d] Monitoring stopped\n", pInfo->local_rank);
    }
}

void free_mem_master() {
    free(comm_mat);
    //free(node_cpus);
    free(pairs);
    free(task_loads);
    free(task_loads_prev);
    free(task_loads_cmp);
    free(pairs_prev);
    free(node_core_start);
    free(task_core);
    free(task_core_prev);
    free(task_pids);
    free(numa_cores);
    free(numa_cores_node);
    free(is_mapped);
}

void get_proc_info(orte_proc_info_t orte_proc_info) {
    //int id, ret;
    //const char **tmp = NULL;
    //const int *tmp = NULL;
    //char **lines = NULL;
    //OMPI_DECLSPEC void jupdate_commmat_shm(const char *shm_name, size_t *data, int np);
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

    //printf("[DeLoc-%d] pid=%d, num_peers=%d, local_rank=%d\n", pInfo->node_rank,
    //        pInfo->pid, pInfo->num_local_peers, pInfo->local_rank);

}

void map_proc(__pid_t pid, int pu_id) {
    int core_id = numa_cores[pu_id];
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    const int set_result = sched_setaffinity(pid, sizeof (cpu_set_t), &cpuset);
    if (set_result != 0) {
        printf("** Err: sched_setaffinity\n");
    }
    /*else {
        printf("** Mapped pid #%d to pu #%d (core-%d)\n", pid, pu_id, core_id);
    }
    */
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
    //long nproc, i;
    //long nproc = sysconf(_SC_NPROCESSORS_ONLN);

    const int ret = sched_getaffinity(pid, sizeof (cpu_set_t), &cpuset);
    if (ret != 0) {
        printf("** Err: get_affinity\n");
    }
    /*
    else {
        //(ret == 0)
        printf("*** CPU affinity: ");
        for (i = 0; i < nproc; i++) {
            printf("%d ", CPU_ISSET(i, &cpuset));
        }
        printf("\n");
    }*/
}

void map_proc_rand(__pid_t pid) {
    int random_core_id;
    random_core_id = 4 + (rand() % 4);
    map_proc(orte_process_info.pid, random_core_id);
}

void update_commmat_shm(const char *shm_name, size_t *data, int np) {
    const int SIZE = sizeof (size_t) * np;
    int shm_fd;
    //int ret;
    size_t *ptr;
    if ((shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666)) >= 0) {
        //ret = ftruncate(shm_fd, SIZE);
        ftruncate(shm_fd, SIZE);
        //if (ret == 0) {
            ptr = (size_t *) mmap(0, SIZE, PROT_WRITE, MAP_SHARED, shm_fd, 0);
            for (int i = 0; i < np; i++) {
                ptr[i] = data[i];
            }
            munmap(ptr, SIZE);
        //}
        //else {
        //    printf("Err: failed ftruncate() shm: %s\n", shm_name);
        //}
        close(shm_fd);
    } else {
        printf("Err: failed writing to shm\n");
    }
}

void del_shm(const char *shm_name) {
    shm_unlink(shm_name);
}

void get_commmat_shm(const char *shm_name, size_t *to_data, int np) {
    const int SIZE = sizeof (size_t) * np;
    int shm_fd;
    //size_t *ptr;
    if ((shm_fd = shm_open(shm_name, O_RDONLY, 0666)) >= 0) {
        //ptr = (size_t *) mmap(0, SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
        void *ptr = mmap(0, SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
        for (int i = 0; i < np; i++) {
            to_data[i] = ((size_t *)ptr)[i];
        }
        munmap(ptr, SIZE);
        close(shm_fd);
    } else {
        printf("Err: failed reading shm\n");
    }
}

void get_all_commmat_shm(const int SIZE) {
    //const int SIZE = sizeof (size_t) * num_local_procs;
    char shm_name[16];
    int shm_fd, r, i;//, j;
    //size_t *ptr;

    reset_comm_mat();
    //reset_prev_comm_mat();
    for (r = 0; r < num_local_procs; r++) {
        snprintf(shm_name, 16, "DELOC-%d", r);
        //printf("Get DELOC-%d\n", r);
        if ((shm_fd = shm_open(shm_name, O_RDONLY, 0666)) >= 0) {
            //ptr = (size_t *) mmap(0, SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
            void *ptr = mmap(0, SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
            for (i = 0; i < num_local_procs; i++) {
                comm_mat[r][i] += ((size_t*)ptr)[i];
                comm_mat[i][r] += ((size_t*)ptr)[i];
                //comm_mat[i][r] += ptr[i];
                //prev_comm_mat[r][i] += ptr[i];
                //prev_comm_mat[i][r] += ptr[i];
            }
            munmap(ptr, SIZE);
            close(shm_fd);
        } else if (!stopDelocMon) {
            //printf("[#%d] Err: failed reading shm\n", pInfo->local_rank);
            printf("[#%d] Err: failed reading comm_mat from shm\n",
                    proc_info.my_local_rank);
        }
    }
    /*
    for (i = 0; i < num_local_procs; i++) {
        for (j = i+1; j < num_local_procs; j++) {
            comm_mat[i][j] = prev_comm_mat[i][j] - comm_mat[i][j];
        }
    }
    */
    //comm_mat_to_pairs(comm_mat, pairs);
    //comm_mat_to_task_loads(comm_mat, task_loads);
    //print_pairs();
}

void get_all_task_shm() {
    //const int SIZE = sizeof (size_t) * num_local_procs;
    //const int SIZE = sizeof (struct info);
    char shm_name[16];
    int shm_fd, r;
    //struct info *ptr;

    for (r = 0; r < num_local_procs; r++) {
        snprintf(shm_name,16, "DELOC-t-%d", r);
        if ((shm_fd = shm_open(shm_name, O_RDONLY, 0666)) >= 0) {
            void *v_ptr = mmap(0, sizeof(struct info), PROT_READ,
                 MAP_SHARED, shm_fd, 0);
            //ptr =  (struct info *) v_ptr;
            //char rank_s[16];
            //snprintf(rank_s, 16, "%d", ptr->local_rank);
            //d_tasks[r].task_id = ptr->pid;
            task_pids[r] = ((struct info *) v_ptr)->pid;
            num_pids++;

            munmap(v_ptr, sizeof(struct info));
            close(shm_fd);
        } else {
            printf("Error: failed reading tasks from shm\n");
        }
    }
    // KeyMap-based implementation
    /*
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
                task_pids[r] = ptr->pid;
                num_pids++;

                close(shm_fd);
            } else {
                printf("Error: failed reading tasks shm\n");
            }
        }
    }
    */
}

void update_task_shm(struct info *p_info) {
    char info_shm[16];
    snprintf(info_shm, 16, "DELOC-t-%d", pInfo->local_rank);
    const int SIZE = sizeof (struct info);
    int shm_fd; //int ret;
    struct info *ptr;
    if ((shm_fd = shm_open(info_shm, O_CREAT | O_RDWR, 0666)) >= 0) {
        //ret = ftruncate(shm_fd, SIZE);
        ftruncate(shm_fd, SIZE);
        //if (ret == 0) {
            ptr = (struct info *) mmap(0, SIZE, PROT_WRITE, MAP_SHARED, shm_fd, 0);
            ptr->local_rank = p_info->local_rank;
            ptr->node_rank = p_info->node_rank;
            ptr->num_local_peers = p_info->num_local_peers;
            ptr->pid = p_info->pid;
            ptr->ts = p_info->ts;

            munmap(ptr, SIZE);
        //}
        //else {
        //    printf("Err: failed ftruncate() shm: %s\n", info_shm);
        //}
        close(shm_fd);
    } else {
        printf("Err: failed writing task info to shm\n");
    }
}

void update_my_pid_shm() {
    //char info_shm[16];
    //snprintf(info_shm, 16, "DELOC-p-%d", orte_proc_info.my_local_rank);
    int shm_fd;
    if ((shm_fd = shm_open(shm_name_pid, O_CREAT | O_RDWR, 0666)) >= 0) {
        if (ftruncate(shm_fd, SIZE_PID) == 0) {
            __pid_t *ptr = (__pid_t *) mmap(0, SIZE_PID, PROT_WRITE, MAP_SHARED, shm_fd, 0);
            *ptr = proc_info.pid;
            munmap(ptr, SIZE_PID);
        }
        else {
            printf("[DeLoc-%d] ERR: shm failed ftruncate() pid\n", proc_info.my_local_rank);
        }
        close(shm_fd);
    } else {
        printf("[DeLoc-%d] ERR: failed writing pid to shm\n", proc_info.my_local_rank);
    }
}

void get_all_pids_shm() {
    char shm_name[SHM_N_LEN];
    int shm_fd, r;

    for (r = 0; r < num_tasks; r++) {
        snprintf(shm_name, SHM_N_LEN, "DELOC-p-%d", r);
        if ((shm_fd = shm_open(shm_name, O_RDONLY, 0666)) >= 0) {
            void *ptr = mmap(0, SIZE_PID, PROT_READ, MAP_SHARED, shm_fd, 0);
            task_pids[r] = *(__pid_t *) ptr;
            num_pids++;

            munmap(ptr, SIZE_PID);
            close(shm_fd);
        } else {
            printf("[DeLoc] ERR: failed reading pids from shm\n");
        }
    }
}

void update_poll_itv_shm(unsigned long new_itv) {
    int shm_fd; 
    if ((shm_fd = shm_open(SHM_POLL_ITV, O_CREAT | O_RDWR, 0666)) >= 0) {
        if (ftruncate(shm_fd, SIZE_POLL_ITV) == 0) {
            unsigned long *ptr = (unsigned long *) mmap(0, SIZE_POLL_ITV,
                 PROT_WRITE, MAP_SHARED, shm_fd, 0);
            *ptr = new_itv;
            munmap(ptr, SIZE_POLL_ITV);
        }
        else {
            printf("Err: shm failed ftruncate(): pollInterval\n");
        }
        close(shm_fd);
    } else {
        printf("Err: failed writing pollInterval to shm\n");
    }
}

void get_poll_itv_shm() {
    //unsigned long new_itv = pollInterval;
    int shm_fd;
    
    if ((shm_fd = shm_open(SHM_POLL_ITV, O_RDONLY, 0666)) >= 0) {
        //ret = ftruncate(shm_fd, SIZE);
        //if (ftruncate(shm_fd, sizeof(unsigned long)) == 0) {
        void *ptr = mmap(0, SIZE_POLL_ITV,
                 PROT_READ, MAP_SHARED, shm_fd, 0);
        if (ptr != MAP_FAILED) {
            pollInterval = *(unsigned long *) ptr;
        }
        munmap(ptr, SIZE_POLL_ITV);
        close(shm_fd);
    } else {
        printf("Err: failed reading pollInterval from shm\n");
    }
}

void comm_mat_to_pairs(size_t **mat, struct pair *pairs) {
    int i, j, r;
    j = 0;
    for (r = 0; r < num_local_procs; r++) {
        for (i = r + 1; i < num_local_procs; i++) {
            pairs[j].t1 = r;
            pairs[j].t2 = i;
            pairs[j].ncomm = mat[r][i];
            j++;
        }
    }
    //print_pairs(pairs);
    //print_pairs(pairs_prev);
}

void comm_mat_to_pairs_tasks(size_t **mat, struct pair *pairs,
     struct loadObj *task_loads) {
    int i, j, r, t;
    j = t = 0;
    for (r = 0; r < num_tasks; r++) {
        task_loads[t].id = r;
        task_loads[t].load = 0;
        for (i = r + 1; i < num_tasks; i++) {
            // loads
            task_loads[t].load += mat[r][i];
            // pairs
            pairs[j].t1 = r;
            pairs[j].t2 = i;
            pairs[j].ncomm = mat[r][i];
            j++;
        }
        for (i = 0; i < r; i++) {
            // loads of lower ranks
            task_loads[t].load += mat[r][i];
        }
        t++;
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

int compare_loadObj_id(const void *a, const void *b) {
    const struct loadObj *ia = (const struct loadObj *) a; // casting pointer types 
    const struct loadObj *ib = (const struct loadObj *) b;
    return ia->id - ib->id;
}

void comm_mat_to_task_loads(size_t **mat, struct loadObj *task_loads) {
    int i, j, t;
    t = 0;
    for (i = 0; i < num_tasks; i++) {
        task_loads[t].id = i;
        task_loads[t].load = 0;
        for (j = 0; j < num_tasks; j++) {
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

void reset_node_loads() {
    for (int i = 0; i < num_nodes; i++) {
        node_loads[i].load = 0;
    }
}

/*
void print_node_cpus() {
    for (int i = 0; i < num_nodes; i++) {
        for (int j = 0; j < n_cores_per_node; j++) {
            printf("\tNode-%d, core-%d -> task-%d\n", i, j, node_cpus[i][j]);
        }
    }
}
*/

void print_task_core() {
    for (int i = 0; i < num_tasks; i++) {
        printf("\tTask-%d => core-%d\n", i, task_core[i]);
    }
}

void update_task_core_prev() {
    for (int i = 0; i < num_tasks; i++) {
        task_core_prev[i] = task_core[i];
    }
}

void print_task_loads() {
    for (int i = 0; i < num_tasks; i++) {
        printf("\tTask-%d: %lu\n", task_loads[i].id, task_loads[i].load);
    }
    putchar('\n');
}

void print_pairs(struct pair *ps, int n_p) {
    printf("Number of process pairs: %d\n", n_p);
    for (int i = 0; i < n_p; i++) {
        if (ps[i].t1 != ps[i].t2 && ps[i].ncomm > 0) {
            printf(" %d-%d:%zu", ps[i].t1, ps[i].t2, ps[i].ncomm);
        }
    }
    putchar('\n');
}

int ser_core_to_node(int core_id) {
    //printf("** ser_core: %d\n", core_id);
    //int div = core_id / n_cores_per_node;
    /*
    int mod = core_id % n_cores_per_node;
    if (mod > 0) {
        div++;
    }*/
    //return (div);
    return core_id / n_cores_per_node;
}

int node_core_to_ser(int node_id, int core_id) {
    return (node_id * n_cores_per_node) +core_id;
}

void get_numa_cpus() {
    int i, k, c, err;
    unsigned j;
    struct bitmask *cpus;
    
    c = 0;
    for (i = 0; i < num_nodes; i++) {
        cpus = numa_allocate_cpumask();
        err = numa_node_to_cpus(i, cpus);
        if (err >= 0) {
            k = j = 0;
            while (j < cpus->size && k < n_cores_per_node) {
                if (numa_bitmask_isbitset(cpus, j)) {
                    numa_cores[c] = j;
                    numa_cores_node[c] = i;
                    c++;
                    k++;
                }
                j++;
            }
            /*
            for (j = 0; j < cpus->size; j++) {
                if (numa_bitmask_isbitset(cpus, j)) {
                    numa_cores[c++] = j;
                }
            }
            */
        }
    }
}

void print_numa_node_cpus(int node) {
    int err;
    unsigned i;
    struct bitmask *cpus;

    cpus = numa_allocate_cpumask();
    err = numa_node_to_cpus(node, cpus);
    if (err >= 0) {
        for (i = 0; i < cpus->size; i++) {
            if (numa_bitmask_isbitset(cpus, i)) {
                printf(" %d", i);
            }
        }
    }
    putchar('\n');
}

void print_comm_mat(size_t **mat) {
    int i,j;
    printf("** Communication matrix:\n\t");
    for (i = 0; i < num_local_procs; i++) {
        for (j = 0; j < num_local_procs; j++) {
            printf("%d ", (unsigned int) mat[i][j]);
        }
        printf("\n\t");
    }
}

/**
 * Pring comm. matrix in DeLoc format
 */
void save_comm_mat(int nbtasks) {
    save_comm_mat_to_file(nbtasks, DELOC_COMM_FILE);
    /*
    int i, j;
    FILE *of = fopen(DELOC_COMM_FILE, "w");

    i = nbtasks - 1;
    for (i = nbtasks - 1; i >= 0; i--) {
        fprintf(of, "%lu", comm_mat[i][0]);
        for (j = 1; j < nbtasks; j++) {
           fprintf(of, ",%lu", comm_mat[i][j]);
        }
        fprintf(of, "\n");
    }
    fclose(of);
    printf("Saved comm. matrix to %s\n", DELOC_COMM_FILE);
    */
}

void save_comm_mat_to_file(int nbtasks, const char* out_filename) {
    int i, j;
    FILE *of = fopen(out_filename, "w");

    i = nbtasks - 1;
    for (i = nbtasks - 1; i >= 0; i--) {
        fprintf(of, "%lu", comm_mat[i][0]);
        for (j = 1; j < nbtasks; j++) {
           fprintf(of, ",%lu", comm_mat[i][j]);
        }
        fprintf(of, "\n");
    }
    fclose(of);
    printf("Saved comm. matrix to %s\n", out_filename);
}

void save_comm_mat_part(int nbtasks, int part) {
    //char *part_st = atoi(part);
    //char *fname = DELOC_COMM_FILE;
    //strcat()
    char fname[32];
    snprintf(fname, 32, "%s.%d", DELOC_COMM_FILE, part);
    save_comm_mat_to_file(nbtasks, fname);
}

int compare_update_pairs(struct pair *p1, struct pair *p2, int n_p) {
    int diff = 0;
    for (int i = 0; i < n_p; i++) {
        if (p1[i].t1 != p2[i].t1 ||
            p1[i].t2 != p2[i].t2) {
            diff++;
        }
        p2[i].t1 = p1[i].t1;
        p2[i].t2 = p1[i].t2;
    }
    return diff;
}

int compare_update_task_loads(struct loadObj *l1, struct loadObj *l2, int n_l) {
    int diff = 0;
    for (int i = 0; i < n_l; i++) {
        if (l1[i].id != l2[i].id) {
            diff++;
        }
        l2[i].id = l1[i].id;
        //l2[i].load = l1[i].load;
    }
    return diff;
}

int compare_update_task_most(struct loadObj *cur, struct loadObj *prev,
        int n, int n_changed) {
    int i, diff;//, same;
    unsigned before;
    //int diff = 0;
    //struct loadObj *tmp;
    //printf("Comparing update task most..\n");
    //printf("Tasks_most: ");
    qsort(cur, n, sizeof (struct loadObj), compare_loadObj_rev);
    qsort(cur, ntasks_prev, sizeof (struct loadObj), compare_loadObj_id);
    
    /*
    if (n_changed == 0) {
        for (i = 0; i < ntasks_prev; i++) {
            prev[i].id = cur[i].id; 
        }
        diff = num_tasks;
    }
    else {
    */
        //qsort(cur, n, sizeof (struct loadObj), compare_loadObj_id);
        //tmp = (struct loadObj *) malloc(npairs_prev * sizeof (struct loadObj));
        //memcpy(tmp, cur, npairs_prev*sizeof(struct loadObj));
        //qsort(tmp, npairs_prev, sizeof (struct loadObj), compare_loadObj_id);
        //int n_merge = ntasks_prev*2;
        //struct loadObj *tmp = (struct loadObj *) malloc (n_merge*sizeof(struct loadObj));
        for (i = 0; i < ntasks_prev; i++) {
            task_loads_cmp[i].id = cur[i].id;
            task_loads_cmp[i + ntasks_prev].id = prev[i].id;
            prev[i].id = cur[i].id; 
        }
        /*
        for (i = 0; i < ntasks_prev; i++) {
            task_loads_cmp[i+ntasks_prev].id = prev[i].id;
            prev[i].id = cur[i].id; 
        }
        */
        qsort(task_loads_cmp, ntasks_cmp, sizeof(struct loadObj), compare_loadObj_id);
    
        //same = 0;
        diff = ntasks_cmp;
        before = task_loads_cmp[0].id;
        i = 1;
        //printf("Tmp tasks most: %d ", before);
        while (i < ntasks_cmp) {
            //printf("%d ", task_loads_cmp[i].id);
            if (task_loads_cmp[i].id == before) {
                //same += 2;
                diff -= 2;
            }
            before = task_loads_cmp[i].id;
            i++;
        }
        //putchar('\n');
        //free(tmp);
        //diff = ntasks_cmp - same;
    //}

    //printf("Diff: %d\n", diff);
    return diff;

    /*
    for (int i = 0; i < ntasks_prev; i++) {
        //printf("%d:%lu ", cur[i].id, cur[i].load);
        //printf("%d ", cur[i].id);
        
        if (cur[i].id != prev[i].id) {
            diff++;
        }
        prev[i].id = cur[i].id;
        /
        if (tmp[i].id != prev[i].id) {
            diff++;
        }
        prev[i].id = tmp[i].id;
        /
    }*/
    //putchar('\n');
    //free(tmp);
    //printf("Finished compare update task most..\n");
    //return diff;
}

int map_to_next_core(int node_id, int task_id) {
    int target_node = -1;
    int target_core = -1;
    if (is_avail(node_id)) {
        target_node = node_id;
        target_core = node_core_start[target_node];
        //node_cpus[node_id][target_core] = task_id;
        node_core_start[target_node]++;
    } else {
        int tried = 0;
        int cand_node = next_node(node_id);
        while ((target_node == -1) && (tried < num_nodes)) {
            if (is_avail(cand_node)) {
                target_node = cand_node;
                target_core = node_core_start[target_node];
                //node_cpus[target_node][target_core] = task_id;
                node_core_start[target_node]++;
            }
            tried++;
            cand_node = next_node(cand_node);
        }
    }
    if (target_node > -1) {
        task_core[task_id] = node_core_to_ser(target_node, target_core);
    }
    else {
        printf("Err: cannot map to any node: %d\n", task_id);
    }
    return target_node;
}

/*
int map_to_next_core_if(int node_id, int task_id) {
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
                node_cpus[target_node][target_core] = task_id;
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
*/

void map_deloc() {
    int i, n_mapped;
    // int mult;
    /*
    node_core_start = (int *) malloc(num_nodes * sizeof (int));
    for (i = 0; i < num_nodes; i++) {
        node_core_start[i] = 0;
    }
    */
    reset_node_core_start();

    //bool is_mapped[num_tasks];
    for (i = 0; i < num_tasks; i++) {
        is_mapped[i] = false;
    }

    //printf("* Run DeLoc mapping.. \n");
    n_mapped = 0;
    /*
    qsort(pairs, npairs, sizeof (struct pair), compare_pair);
    
    print_pairs(pairs_prev, npairs_prev);
    print_pairs(pairs, npairs_prev);
    if (compare_pairs(pairs, pairs_prev, npairs_prev) > n_cores_per_node) {
        printf("** Pairs changed\n");
    }
    for (int i=0; i < npairs_prev; i++) {
        pairs_prev[i].t1 = pairs[i].t1;
        pairs_prev[i].t2 = pairs[i].t2;
        pairs_prev[i].ncomm = pairs[i].ncomm;
    }*/
    //mult = 0;
    i = 0;
    //int curr_node = 0;
    while (n_mapped < num_tasks && i < npairs) {
        struct pair p = pairs[i];
        //int curr_node = mult % num_nodes;
        //int t1_node = curr_node;
        int t1_node = i % num_nodes;
        //printf("** curr_node=%d, curr_pair: %d-%d (%ld)\n", t1_node, p.t1,
        //        p.t2, p.ncomm);

        if (!is_mapped[p.t1]) {
            t1_node = map_to_next_core(t1_node, p.t1);
            if (t1_node > -1) {
                n_mapped++;
                is_mapped[p.t1] = true;
            }
        } else {
            // Mapped before
            t1_node = ser_core_to_node(task_core[p.t1]);
            //printf("T1 mapped before to: %d\n", t1_node);
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
    //print_node_cpus();
    //print_task_core();
}

void map_deloc_if() {
    int i, n_mapped, t1_node;
    
    reset_node_core_start();

    //bool is_mapped[num_tasks];
    for (i = 0; i < num_tasks; i++) {
        is_mapped[i] = false;
    }

    n_mapped = 0;
    i = 0;
    //t1_node = numa_cores_node[task_core_prev[p.t1]];
    while (n_mapped < num_tasks && i < npairs) {
        struct pair p = pairs[i];

        if (!is_mapped[p.t1]) {
            t1_node = numa_cores_node[task_core_prev[p.t1]];
            t1_node = map_to_next_core(t1_node, p.t1);
            if (t1_node > -1) {
                n_mapped++;
                is_mapped[p.t1] = true;
            }
        } else {
            // Mapped before
            t1_node = ser_core_to_node(task_core[p.t1]);
        }
        if (!is_mapped[p.t2]) {
            t1_node = map_to_next_core(t1_node, p.t2);
            if (t1_node > -1) {
                n_mapped++;
                is_mapped[p.t2] = true;
            }
        }
        i++;
    }
}

void map_balance() {
    int t, n;
    /*
    node_core_start = (int *) malloc(num_nodes * sizeof (int));
    for (n = 0; n < num_nodes; n++) {
        node_core_start[n] = 0;
    }*/
    reset_node_core_start();

    // Reset node loads
    /*
    node_loads = (struct loadObj*) malloc(num_nodes * sizeof (struct loadObj));
    for (n = 0; n < num_nodes; n++) {
        node_loads[n].id = n;
        node_loads[n].load = 0;
    }*/
    reset_node_loads();

    //printf("* Running Balance mapping.. \n");
    t = 0;
    //n = 0;
    //qsort(task_loads, num_tasks, sizeof (struct loadObj), compare_loadObj_rev);
    while (t < num_tasks) {
        n = node_loads[0].id;
        //int target_node = map_to_next_core(n, task_loads[t].id);
        map_to_next_core(n, task_loads[t].id);
        //printf("** Least node=%d, Task-%d->Node-%d\n", n, task_loads[t].id,
        //        target_node);
        node_loads[0].load += task_loads[t].load;
        qsort(node_loads, num_nodes, sizeof (struct loadObj), compare_loadObj);
        //        for (int i = 0; i < num_nodes; i++) {
        //            printf("** Node[%d]=%ld\n", node_loads[i].id, node_loads[i].load);
        //        }
        t++;
    }

    //print_node_cpus();
}

void map_locality() {
    int t, n;
    reset_node_core_start();

    //qsort(task_loads, num_tasks, sizeof (struct loadObj), compare_loadObj_rev);
    t = 0;
    n = 0;
    while (t < num_tasks) {
        //int target_node = map_to_next_core(n, task_loads[t].id);
        map_to_next_core(n, task_loads[t].id);
        if (!is_avail(n)) {
            n++;
        }
        t++;
    }
}

void map_deloc_tl() {
    int t, n, target_node;
    size_t avg_load;
    /*
    node_core_start = (int *) malloc(num_nodes * sizeof (int));
    for (n = 0; n < num_nodes; n++) {
        node_core_start[n] = 0;
    }*/
    reset_node_core_start();

    // Reset node loads
    /*
    node_loads = (struct loadObj*) malloc(num_nodes * sizeof (struct loadObj));
    for (n = 0; n < num_nodes; n++) {
        node_loads[n].id = n;
        node_loads[n].load = 0;
    }*/
    reset_node_loads();
    
    avg_load = 0;
    for (t = 0; t < num_tasks; t++) {
        avg_load += task_loads[t].load;
    }
    avg_load = avg_load / num_nodes;

    //printf("* Running DeLocTL mapping.. \n");
    t = 0;
    //n = 0;
    //qsort(task_loads, num_tasks, sizeof (struct loadObj), compare_loadObj_rev);
    n = 0;
    while (t < num_tasks) {
        target_node = map_to_next_core(n, task_loads[t].id);
        //printf("** Least node=%d, Task-%d->Node-%d\n", n, task_loads[t].id,
        //        target_node);
        node_loads[target_node].load += task_loads[t].load;
        if (node_loads[n].load >= avg_load) {
            n = next_node(n);
        }
        //qsort(node_loads, num_nodes, sizeof (struct loadObj), compare_loadObj);
        //        for (int i = 0; i < num_nodes; i++) {
        //            printf("** Node[%d]=%ld\n", node_loads[i].id, node_loads[i].load);
        //        }
        t++;
    }
    //print_node_cpus();
}
