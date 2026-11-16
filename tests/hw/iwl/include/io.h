/* io_wait on the host: about a microsecond of card time, as the write
   to port 0x80 takes on the machine */
#ifndef HW_IWL_IO_H
#define HW_IWL_IO_H
void model_io_wait(void);
static inline void io_wait(void) { model_io_wait(); }
#endif
