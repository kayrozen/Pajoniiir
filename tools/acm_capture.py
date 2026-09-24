import serial, time, sys

s = serial.Serial('/dev/ttyACM0', 115200, timeout=2)
end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 30)
buf = b''
while time.time() < end:
    buf += s.read(4096)
open('/host/tmp_boot_log.txt', 'wb').write(buf)
print('captured', len(buf))
