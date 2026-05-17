import subprocess
import time
import os

def run_test():
    # Start the node process
    proc = subprocess.Popen(['./nos_ProcA'], 
                            stdin=subprocess.PIPE, 
                            stdout=subprocess.PIPE, 
                            stderr=subprocess.STDOUT, 
                            text=True,
                            bufsize=0)

    print("--- Node started ---")
    
    # Wait for the node to initialize and for heartbeats to occur
    # Timer is 3s, so let's wait 10s for ~3 heartbeats
    time.sleep(10)
    
    # Send 'show db' to see the state
    proc.stdin.write("show db\n")
    proc.stdin.flush()
    time.sleep(1)
    
    # Send 'reload Comp-2'
    print("--- Reloading Comp-2 ---")
    proc.stdin.write("reload Comp-2\n")
    proc.stdin.flush()
    time.sleep(5) # Wait for another heartbeat
    
    # Final check
    proc.stdin.write("show db\n")
    proc.stdin.flush()
    time.sleep(1)
    
    # Exit
    proc.stdin.write("quit\n")
    proc.stdin.flush()
    
    # Capture and print output
    output, _ = proc.communicate(timeout=5)
    print(output)

if __name__ == "__main__":
    run_test()
