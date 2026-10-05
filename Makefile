CC = gcc
CFLAGS = -O3 -march=native -ffast-math -std=c11 -static-libgcc

.PHONY: all clean run

all: apollo_1_0_infer.exe apollo_teacher_infer.exe apollo_1_0_chat.exe apollo_compact_infer.exe apollo_compact_chat.exe

apollo_1_0_infer.exe: src/brain_runtime.c src/gpu_matvec.c src/gpu_matvec.h
	$(CC) $(CFLAGS) -DAPOLLO_COMPACT -municode src/brain_runtime.c src/gpu_matvec.c -o $@ -lm -ld3d12 -ld3dcompiler -ldxgi

apollo_teacher_infer.exe: src/brain_runtime.c src/gpu_matvec.c src/gpu_matvec.h
	$(CC) $(CFLAGS) -municode src/brain_runtime.c src/gpu_matvec.c -o $@ -lm -ld3d12 -ld3dcompiler -ldxgi

apollo_compact_infer.exe: src/brain_runtime.c src/gpu_matvec.c src/gpu_matvec.h
	$(CC) $(CFLAGS) -DAPOLLO_COMPACT -municode src/brain_runtime.c src/gpu_matvec.c -o $@ -lm -ld3d12 -ld3dcompiler -ldxgi

apollo_1_0_chat.exe: src/chat_gui.c
	$(CC) -O2 -std=c11 -static-libgcc src/chat_gui.c -o $@ -mwindows

apollo_compact_chat.exe: src/chat_gui.c
	$(CC) -O2 -std=c11 -static-libgcc -DAPOLLO_COMPACT src/chat_gui.c -o $@ -mwindows

run: all
	./apollo_1_0_chat.exe

clean:
	rm -f apollo_1_0_infer.exe apollo_teacher_infer.exe apollo_1_0_chat.exe apollo_compact_infer.exe apollo_compact_chat.exe
