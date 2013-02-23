/**
 * @file
 * @brief
 *
 * @date 13.02.12
 * @author Anton Bulychev
 */

#include <kernel/percpu.h>

struct thread;

/* Defining current thread for each CPU */
struct thread *__current_thread __percpu__;
