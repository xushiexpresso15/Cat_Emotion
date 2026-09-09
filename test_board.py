import argparse
import serial
import time

parser = argparse.ArgumentParser(description="Query a Grove Vision AI V2 board")
parser.add_argument("--port", default="/dev/ttyACM0")
args = parser.parse_args()

conn = serial.Serial(args.port, 921600, timeout=1)
time.sleep(0.5)
conn.reset_input_buffer()
conn.reset_output_buffer()
conn.write(b'AT+BREAK\r\n')
time.sleep(0.05)
conn.reset_input_buffer()
conn.write(b'AT+MODEL?\r\n')

start = time.time()
while time.time() - start < 3:
    if conn.in_waiting:
        line = conn.readline().decode('utf-8', errors='ignore')
        print("BOARD:", line.strip())
conn.close()
