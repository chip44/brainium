#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ring.h"

#define SHM_NAME "/brainium_shm"

ring_buf_t *shm_init(bool init);
void shm_exit(ring_buf_t **ring);
