
# Format based on www.cs.bu.edu/teaching/cpp/writing-makefiles/
# *****************************************************
# G++ Variables

CC = gcc
CFLAGS += -Wall -g

# ****************************************************
# Objects required for compilation/executable

basicShell: basicShell.o
	$(CC) -o basicShell basicShell.o $(CFLAGS)

basicShell.o:

clean:
		-rm -rf *.o basicShell