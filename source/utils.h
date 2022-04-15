#pragma once
#include <aio.h>

void randname(char *buffer);

int create_shm_file();

int allocate_shm_file(size_t size);
