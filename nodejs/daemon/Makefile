_CXXFLAGS = -std=c++0x -O2 -Wall -pthread -fstack-protector-strong
_CPPFLAGS = -DNDEBUG
_LDFLAGS = -Wl,-rpath=. -pthread -s -Wl,-z,now -Wl,-z,relro
_LDLIBS = -luv -lv8 -lv8_libbase -lv8_libplatform -licuuc -licui18n -lc++ -lstdc++

jsjaild: main.o
	$(CC) $(_LDFLAGS) $(LDFLAGS) -o $@ $^ $(LOADLIBES) $(_LDLIBS) $(LDLIBS)

main.o: main.cpp
	$(CXX) $(_CPPFLAGS) $(CPPFLAGS) $(_CXXFLAGS) $(CXXFLAGS) -c -o $@ $<
