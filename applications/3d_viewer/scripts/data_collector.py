from ..pymi import ViewerClient

client = ViewerClient()

# Ensure the connection is alive
client.ping()

client.set_suspended(True)
# Capture many frames
for i in range(100):
    meta, data = client.export_frame(timeout=10.0)
    print(f"Captured frame {i}: meta={meta}, data size={len(data)} bytes")
    time.sleep(0.1)  # Slight delay between captures
    client.next_frame(timeout=10.0)

