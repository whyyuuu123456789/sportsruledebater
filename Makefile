# The council, the video review and the website, plus the tests. Needs a C++20 compiler and libcurl.
#
#   make            build everything into build/
#   make test       build and run the tests (scripted models, no LLM needed)
#   make clean

CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -Isrc -MMD -MP
LDLIBS += -lcurl -lpthread

LIB_SRC := $(filter-out src/main_%.cpp,$(wildcard src/*.cpp))
LIB_OBJ := $(LIB_SRC:src/%.cpp=build/obj/%.o)
TEST_OBJ := $(patsubst tests/%.cpp,build/obj/tests/%.o,$(wildcard tests/*.cpp))
PROGRAMS := build/council build/review build/web

all: $(PROGRAMS) build/tests

build/council: build/obj/main_council.o $(LIB_OBJ)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/review: build/obj/main_review.o $(LIB_OBJ)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/web: build/obj/main_web.o $(LIB_OBJ)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/tests: $(TEST_OBJ) $(LIB_OBJ)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/obj/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -Itests $(CXXFLAGS) -c $< -o $@

test: build/tests
	build/tests

clean:
	rm -rf build

.PHONY: all test clean

-include $(LIB_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(PROGRAMS:build/%=build/obj/main_%.d)
