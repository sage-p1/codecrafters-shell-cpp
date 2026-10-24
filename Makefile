CXX := g++
CXXFLAGS := -Wall -g -std=c++23 -lreadline
LDFLAGS := -L/opt/homebrew/opt/readline/lib
CPPFLAGS := -I/opt/homebrew/opt/readline/include
run: main.cpp 
	$(CXX) $(LDFLAGS) $(CPPFLAGS) $(CXXFLAGS) main.cpp -o main
clean:
	rm main

