/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   deloc_map.h
 * Author: agung
 *
 * Created on March 14, 2019, 4:14 PM
 */

#ifndef MCA_DELOC_MAP_H
#define MCA_DELOC_MAP_H

#include <ompi_config.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <pthread.h>
#include <hwloc.h>
#include "map.h"

BEGIN_C_DECLS

struct info {
    __pid_t pid;
    int node_rank;
    int local_rank;
    int num_local_peers;
    char shm_name[16];
    long ts;
};

struct pair {
    unsigned t1;
    unsigned t2;
    size_t ncomm;
};

struct d_task {
    unsigned task_id;
    __pid_t pid;
    unsigned core_id;
    size_t load;
};

struct loadObj {
    unsigned id;
    size_t load;
};

//typedef map_t(unsigned int) uint_map_t;
typedef map_t(__pid_t) pid_map_t;

bool stopDelocMon;
pthread_t delocThread;
struct info * pInfo;
//size_t *prev_pml_data;
size_t **comm_mat;
size_t **prev_comm_mat;
int num_local_procs;
int num_cores;
int num_nodes;
hwloc_topology_t hw_topo;
//uint_map_t m;
struct pair *pairs;
struct pair *pairs_prev;
struct d_task *d_tasks;
int npairs;
int npairs_prev;
pid_map_t proc_pid_maps;
unsigned *cur_mapping;
int num_pids;
__pid_t *task_pids;

// Polling interval in seconds
int pollInterval;
int pollNMax;
unsigned deloc_enabled;
int n_comm_changed;

OMPI_DECLSPEC void stop_deloc(void );
OMPI_DECLSPEC void get_proc_info(orte_proc_info_t orte_proc_info);
OMPI_DECLSPEC void map_proc(__pid_t pid, int core_id);
OMPI_DECLSPEC void map_rank(unsigned rank_id, int core_id);
OMPI_DECLSPEC void map_proc_rand(__pid_t pid);
OMPI_DECLSPEC void init_commmat_shm(int np, const char *shm_name);
OMPI_DECLSPEC void del_shm(const char *shm_name);
OMPI_DECLSPEC void update_commmat_shm(const char *shm_name, size_t *data, int np);
OMPI_DECLSPEC void update_task_shm(struct info *task_info);
OMPI_DECLSPEC void get_commmat_shm(const char *shm_name, size_t *to_data, int np);
OMPI_DECLSPEC void get_all_commmat_shm(void );
OMPI_DECLSPEC void get_all_task_shm(void );
OMPI_DECLSPEC void init_deloc(orte_proc_info_t orte_proc_info, size_t * pml_data);
OMPI_DECLSPEC void reset_comm_mat(void );
OMPI_DECLSPEC void reset_prev_comm_mat(void );
OMPI_DECLSPEC void comm_mat_to_pairs(size_t **mat, struct pair *pairs);
OMPI_DECLSPEC void get_proc_affinity(__pid_t pid);
void *monitor_exec(void *args);

// DeLocMap
//int num_tasks, num_nodes, num_cores, n_cores_per_node;
int num_tasks;
// NUMA machine representation: 2d array of nodes and cores
int **node_cpus;
int *node_core_start;
int *task_core;
int num_tasks, n_cores_per_node;
struct loadObj *node_loads;
struct loadObj *task_loads;
struct loadObj *task_loads_prev;
// logical sequential => physical core ids
int *numa_cores;

OMPI_DECLSPEC int map_to_next_core(int node_id, int task_id);
OMPI_DECLSPEC void map_deloc(void );
OMPI_DECLSPEC void map_deloc_tl(void );
OMPI_DECLSPEC void map_balance(void );
OMPI_DECLSPEC int compare_pair(const void * a, const void * b);
OMPI_DECLSPEC int compare_task(const void *a, const void *b);
OMPI_DECLSPEC int compare_loadObj(const void *a, const void *b);
OMPI_DECLSPEC int compare_loadObj_rev(const void *a, const void *b);
OMPI_DECLSPEC void comm_mat_to_pairs(size_t **mat, struct pair *pairs);
OMPI_DECLSPEC void comm_mat_to_task_loads(size_t **mat, struct loadObj *task_loads);
OMPI_DECLSPEC bool is_avail(int node_id);
OMPI_DECLSPEC int free_cpu(int node_id);
OMPI_DECLSPEC int next_node(int node_id);
OMPI_DECLSPEC void reset_node_core_start(void );
OMPI_DECLSPEC void reset_node_loads(void );
OMPI_DECLSPEC void print_node_cpus(void );
OMPI_DECLSPEC void print_numa_node_cpus(int node);
OMPI_DECLSPEC void print_task_core(void );
OMPI_DECLSPEC void print_pairs(struct pair *pairs, int n_p);
OMPI_DECLSPEC void get_numa_cpus(void );
OMPI_DECLSPEC int ser_core_to_node(int core_id);
OMPI_DECLSPEC int node_core_to_ser(int node_id, int core_id);
OMPI_DECLSPEC int compare_update_pairs(struct pair *p1, struct pair *p2, int n_p);
OMPI_DECLSPEC int compare_update_task_loads(struct loadObj *l1, struct loadObj *l2, int n_l);

END_C_DECLS

#endif /* DELOC_MAP_H */

