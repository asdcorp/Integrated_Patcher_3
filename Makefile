OPT=-O3 -s -Wall
LDFLAGS=-nostartfiles -lmsvcrt -limagehlp -lntdll -luser32 -nostdlib -lkernel32 -ladvapi32 -Wl,--exclude-all-symbols -Wl,--enable-stdcall-fixup
CFLAGS=-fno-ident -fno-stack-check -fno-stack-protector

GCC32=gcc -m32 -Wl,-e_dll_main -municode

all: slshim32_aoi.dll

slshim32_aoi.dll: slshim.c
	$(GCC32) $(OPT) $< slshim.def -shared -o $@ $(CFLAGS) $(LDFLAGS)
	strip -s $@

clean:
	rm -f *.dll
