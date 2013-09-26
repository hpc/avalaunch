#ifndef NODE_H
#define NODE_H 1

int node_initialize (void);
int node_finalize (void);
int node_get_id (char const * location);
int node_launch (size_t id);

#endif
