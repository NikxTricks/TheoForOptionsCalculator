
all : theo_pricer

theo_pricer : theo_pricer.cpp Makefile
	mkdir -p out
	g++ -g -std=c++20 -Wall -O3 theo_pricer.cpp -o out/theo_pricer

clean:
	rm theo_pricer

