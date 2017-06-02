GuessAndRumble : CacheFuncs.o EvictionSet.o main.o
	cc -o GuessAndRumble CacheFuncs.o EvictionSet.o main.o -l pthread

main.o : main.c
	cc -c main.c

EvictionSet.o : EvictionSet.c
	cc -c EvictionSet.c

CacheFuncs.o : CacheFuncs.c
	cc -c CacheFuncs.c

clean :
	rm -f CacheFuncs.o EvictionSet.o main.o Guess_and_Rumble
