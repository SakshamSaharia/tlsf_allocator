CXX = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra

all: tests benchmark

tests: tlsf.cpp tlsf.h tests.cpp
	$(CXX) $(CXXFLAGS) tlsf.cpp tests.cpp -o tests

benchmark: tlsf.cpp tlsf.h benchmark.cpp
	$(CXX) $(CXXFLAGS) tlsf.cpp benchmark.cpp -o benchmark

clean:
	rm -f tests benchmark