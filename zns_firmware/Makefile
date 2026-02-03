CC = gcc

# 헤더 경로 설정
# -I. : 현재 폴더 (FreeRTOSConfig.h, policy.h 등)
# -I./port : portmacro.h
CFLAGS = -Wall -pthread -g \
         -I. \
         -I./port \
         -I./FreeRTOS-Kernel/include \
         -DprojCOVERAGE_TEST=0 -D_WINDOWS_=0

# 링커 옵션
LDFLAGS = -lrt -lpthread -lm


SRC = main.c \
      policy_arc.c \
      policy_fifo.c \
      policy_ml_arc.c \
      arc_ml_predictor.c \
      FreeRTOS-Kernel/tasks.c \
      FreeRTOS-Kernel/queue.c \
      FreeRTOS-Kernel/list.c \
      FreeRTOS-Kernel/timers.c \
      FreeRTOS-Kernel/portable/MemMang/heap_3.c \
      port/port.c \
      port/utils/wait_for_event.c

OBJ = $(SRC:.c=.o)

# 빌드 규칙
firmware: $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) firmware