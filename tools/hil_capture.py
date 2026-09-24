import serial, sys, time

PORT = None
for a in sys.argv:
    if a.startswith('/dev/tty'):
        PORT = a
ser = serial.Serial(PORT, 115200, timeout=5)
f = open('/hosttmp/serial_all.log', 'ab')
# marker line so every daemon (re)start is visible in the full log
f.write(f"\n[hil] capture attached {PORT} {time.strftime('%Y-%m-%d %H:%M:%S')}\n".encode())
while True:
    d = ser.read(4096)
    if d:
        f.write(d)
        f.flush()
