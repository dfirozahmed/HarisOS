/* HarisOS user program: checks the ported Lua interpreter (LUA.ELF, third_party/lua) by running it on small scripts (lua -e "...") and
 * looking at how each one ends: a script that raises an error makes lua exit with 1, os.exit(n) with n. That exercises the C library the
 * port stands on: printf / strtod number conversion, math, setjmp / longjmp (pcall), malloc, string functions, time, stdio.
 * Exits with 56 if every script ended as expected, otherwise with the number of the first one that did not. */
#include "libc.h"

typedef struct
{
    const char *code; /* the script */
    int status;       /* the exit status it must end with */
} Case;

static const Case cases[] = {
    /* 1 */ {"os.exit(7)", 7},
    /* 2 */ {"assert(10 // 3 == 3 and 7.5 // 2 == 3.0 and 2^53 == 9007199254740992.0 and 1/0 == math.huge and -7 // 2 == -4)", 0},
    /* 3 */ {"assert(string.format('%5.2f|%g|%d|%x|%s|%q', 3.14159, 0.1, 42, 255, 'hi', 'a\"b') == ' 3.14|0.1|42|ff|hi|\"a\\\\\"b\"')", 0},
    /* 4 */ {"assert(tostring(0.1) == '0.1' and tostring(1e15) == '1e+15' and tostring(2^63) == '9.2233720368548e+18' and tostring(-0.0) == '-0.0')", 0},
    /* 5 */ {"assert(tonumber('0x10') == 16 and tonumber('1e2') == 100.0 and tonumber('  12  ') == 12 and tonumber('0x.8p1') == 1.0 and not tonumber('1e'))", 0},
    /* 6 */ {"assert(math.maxinteger + 1 == math.mininteger and 5 // 0.0 == math.huge and math.tointeger(3.0) == 3 and 3 | 4 == 7 and 1 << 62 == 4611686018427387904)", 0},
    /* 7 */ {"assert(math.floor(-3.5) == -4 and math.ceil(3.2) == 4 and math.fmod(7, 3) == 1 and math.sqrt(16) == 4.0 and math.abs(-2) == 2)", 0},
    /* 8 */ {"assert(math.abs(math.sin(math.pi / 2) - 1) < 1e-15 and math.abs(math.cos(math.pi)  + 1) < 1e-15 and math.abs(math.log(math.exp(3)) - 3) < 1e-14 and 10 ^ 2 == 100.0)", 0},
    /* 9 */ {"local s = 0 for i = 1, 100000 do s = s + i end assert(s == 5000050000)", 0},
    /* 10 */ {"local t = {} for i = 1, 2000 do t[i] = (i * 7919) % 2003 end table.sort(t) for i = 2, #t do assert(t[i - 1] <= t[i]) end", 0},
    /* 11 */ {"local ok, e = pcall(error, 'boom') assert(not ok and e == 'boom') assert(select(2, pcall(function() local x = nil; return x.y end)):find('index'))", 0},
    /* 12 */ {"local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1) return b * 2 end) assert(co(1) == 2 and co(10) == 20)", 0},
    /* 13 */ {"assert(('hello world'):gsub('o', '0') == 'hell0 w0rld' and ('abc'):rep(3, ',') == 'abc,abc,abc' and ('x=1, y=22'):match('y=(%d+)') == '22')", 0},
    /* 14 */ {"assert(table.concat({1, 2, 3}, '-') == '1-2-3' and select('#', table.unpack({1, 2, 3})) == 3 and #string.pack('i4', 7) == 4)", 0},
    /* 15 */ {"assert(load('return 1 + 1')() == 2 and not load('return +'))", 0},
    /* 16 */ {"assert(utf8.char(8364) == '\\xE2\\x82\\xAC' and utf8.len('\\xE2\\x82\\xAC') == 1 and #utf8.char(0x10FFFF) == 4)", 0},
    /* 17 */ {"assert(os.time() > 1700000000 and os.clock() >= 0 and os.date('!%Y-%m-%d %H:%M:%S', 86400) == '1970-01-02 00:00:00' and os.time{year=2000, month=1, day=1, hour=0} == 946684800)", 0},
    /* 18 */ {"local f = assert(io.open('LUA.ELF', 'rb')) assert(f:read(4) == '\\127ELF') assert(f:seek('end') > 100000) f:close()", 0},
    /* 19 */ {"local t = setmetatable({}, {__index = function(t, k) error('deep ' .. k) end}) local ok, e = pcall(function() return t.x end) assert(not ok and e:find('deep x')) assert(select('#', pcall(error)) == 2)", 0},
    /* 20 */ {"local t = {} for i = 1, 3000 do t[i] = {i} end t = nil collectgarbage() assert(collectgarbage('count') < 4000)", 0},
    /* 21 */ {"io.write(string.format('lua says %s %d %.3f\\n', _VERSION, 42, 2/3))", 0},
    /* 22 */ {"error('this one must fail')", 1},
};

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0;
    int count = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < count && !fail; i++)
    {
        const char *args[] = {"lua", "-e", cases[i].code, 0};
        int st = -12345;
        long long pid = sys_spawn("LUA.ELF", args);
        if (pid <= 0 || sys_wait(pid, &st) != pid || st != cases[i].status)
        {
            printf("luatest: script %d ended with %d (spawn %d), wanted %d\n", i + 1, st, (int)pid, cases[i].status);
            fail = i + 1;
        }
    }
    if (fail == 0)
        puts("luatest: all scripts passed");
    sys_exit(fail == 0 ? 56 : fail);
}
