PREFIX ?= /usr
CFLAGS ?= -Wall -O2 -DNDEBUG

build: out+err out+err.helper.so

out+err: CFLAGS+=-DHELPER_SO=\"${PREFIX}/lib/out+err.helper.so\"
out+err: out+err.o

out+err.helper.so: CFLAGS+=-fpic -fvisibility=hidden
out+err.helper.so: helper.o hook_engine/hook_engine.o hook_engine/hde/hde64.o
	$(CC) $^ $(CPPFLAGS) $(CFLAGS) -o $@ -shared -Wl,-init,init

install: out+err out+err.helper.so
	install -Ds out+err ${DESTDIR}${PREFIX}/bin/out+err
	install -Ds out+err.helper.so ${DESTDIR}${PREFIX}/lib/out+err.helper.so

clean:
	rm -f *.o hook_engine/*.o hook_engine/hde/*.o out+err out+err.helper.so
