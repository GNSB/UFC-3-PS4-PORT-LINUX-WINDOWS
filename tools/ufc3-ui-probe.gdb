# Log the UI service commands the online pre-game screens send (ufc:01.00.00:network:pregame:...,
# network:game, online:rivalry, online:h2hseason). Each dispatcher compares its third argument
# (rdx, the command string) against its command names. Hardware breakpoints: at most four.
set pagination off
set confirm off
set logging file out/ufc3-ui-probe.log
set logging overwrite on
set logging enabled on
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIGALRM nostop noprint pass
starti
python
import gdb

class UiCommand(gdb.Breakpoint):
    def stop(self):
        try:
            addr = int(gdb.parse_and_eval('(unsigned long)$rdx'))
            if addr < 0x10000:
                command = '<null %#x>' % addr
            else:
                raw = gdb.selected_inferior().read_memory(addr, 160).tobytes()
                command = raw.split(b'\0', 1)[0].decode('latin-1', 'replace')
        except Exception as error:
            command = '<unreadable %s>' % error
        receiver = int(gdb.parse_and_eval('(unsigned long)$rdi'))
        service = int(gdb.parse_and_eval('(unsigned long)$rsi'))
        print('UI COMMAND', self.location, 'receiver=%#x' % receiver,
              'service=%#x' % service, command, flush=True)
        gdb.execute('bt 5')
        return False

UiCommand('*%#x' % (0x800000000 + 0x2e5df0), type=gdb.BP_HARDWARE_BREAKPOINT)

# Follow the RED/BLUE path. Static disassembly shows request-initial-sync
# branches at 0x2e6361 and calls the service callback at 0x2e63c5. These points
# expose the receiver/service objects and the callback's dispatch target. The
# previous candidate writer/send offsets were not hit, so replace them with
# the actual initial-sync path. Disable each after a few hits to limit output.
class PregamePoint(gdb.Breakpoint):
    def __init__(self, name, offset, limit=8):
        super().__init__('*%#x' % (0x800000000 + offset), type=gdb.BP_HARDWARE_BREAKPOINT)
        self.name = name
        self.limit = limit
        self.hits = 0

    def stop(self):
        self.hits += 1
        regs = {}
        for reg in ('rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9', 'r14', 'r15'):
            try:
                regs[reg] = int(gdb.parse_and_eval('$' + reg))
            except Exception:
                regs[reg] = 0
        print('PREGAME POINT', self.name, 'hit', self.hits,
              'registers', ' '.join('%s=%#x' % item for item in regs.items()), flush=True)
        if self.name == 'network:pregame2' and regs['rdx'] >= 0x100000000:
            try:
                raw = gdb.selected_inferior().read_memory(regs['rdx'], 160).tobytes()
                command = raw.split(b'\0', 1)[0].decode('latin-1', 'replace')
                print('  UI COMMAND', command, flush=True)
            except Exception:
                pass
        for reg in ('rsi', 'rdx', 'rcx', 'r8', 'r9'):
            addr = regs[reg]
            if addr < 0x100000000:
                continue
            try:
                raw = gdb.selected_inferior().read_memory(addr, 48).tobytes()
                print('  %s[0:48] %s' % (reg, raw.hex(' ')), flush=True)
            except Exception:
                pass
        if self.name == 'initial-sync-callback':
            try:
                obj = regs['rdi']
                vtable = int.from_bytes(gdb.selected_inferior().read_memory(obj, 8).tobytes(), 'little')
                target = int.from_bytes(gdb.selected_inferior().read_memory(vtable + 0x38, 8).tobytes(), 'little')
                print('  callback object=%#x vtable=%#x +0x38=%#x data=%#x' %
                      (obj, vtable, target, regs['rdx']), flush=True)
                nested = int.from_bytes(gdb.selected_inferior().read_memory(obj + 0x18, 8).tobytes(), 'little')
                nested_data = int.from_bytes(gdb.selected_inferior().read_memory(obj + 0x20, 8).tobytes(), 'little')
                nested_vtable = int.from_bytes(gdb.selected_inferior().read_memory(nested, 8).tobytes(), 'little') if nested else 0
                nested_target = int.from_bytes(gdb.selected_inferior().read_memory(nested_vtable + 0x20, 8).tobytes(), 'little') if nested_vtable else 0
                print('  forwarded callback object=%#x data=%#x vtable=%#x +0x20=%#x' %
                      (nested, nested_data, nested_vtable, nested_target), flush=True)
                if regs['rdx'] >= 0x100000000:
                    raw = gdb.selected_inferior().read_memory(regs['rdx'], 128).tobytes()
                    print('  callback data[0:128] %s' % raw.hex(' '), flush=True)
            except Exception as error:
                print('  callback inspection failed: %s' % error, flush=True)
        gdb.execute('bt 8')
        if self.hits >= self.limit:
            self.enabled = False
        return False

PregamePoint('network:pregame2', 0x5b7cb0)
for name, offset in (('initial-sync-handler', 0x2e6361),
                     ('initial-sync-callback', 0x2e63c5)):
    PregamePoint(name, offset)
end
continue
