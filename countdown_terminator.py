import psutil
import time
import datetime

# --- 配置 ---
TARGET_PROCESS_NAME = "3d_viewer.exe"  # 目标进程的名称
TIME_LIMIT_SECONDS = 120                # 允许的最长执行时间（秒）
CHECK_INTERVAL_SECONDS = 1             # 检查间隔（秒）
# --------------

def find_and_terminate_process():
    """
    查找并终止运行超时的目标进程。
    """
    # 遍历所有正在运行的进程
    for proc in psutil.process_iter(['pid', 'name', 'create_time']):
        try:
            # 检查进程名是否匹配（不区分大小写）
            if proc.info['name'].lower() == TARGET_PROCESS_NAME.lower():
                # 计算进程已运行的时间
                running_duration = time.time() - proc.info['create_time']
                
                print(f"发现目标进程 '{TARGET_PROCESS_NAME}' (PID: {proc.pid})，已运行 {running_duration:.1f} 秒。")

                # 如果运行时间超过限制，则强制终止
                if running_duration > TIME_LIMIT_SECONDS:
                    print(f"警告：进程 {proc.pid} 运行已超过 {TIME_LIMIT_SECONDS} 秒。正在强制终止...")
                    proc.kill()  # 使用最强制的手段终止进程
                    print(f"进程 {proc.pid} 已被成功终止。")

        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            # 进程可能在我们检查时已经结束，或者我们没有权限访问它
            pass
        except Exception as e:
            print(f"发生意外错误: {e}")

if __name__ == "__main__":
    print("--- 进程超时终止脚本已启动 ---")
    print(f"监控目标: '{TARGET_PROCESS_NAME}'")
    print(f"时间限制: {TIME_LIMIT_SECONDS} 秒")
    print("按 Ctrl+C 停止脚本。")
    
    try:
        while True:
            find_and_terminate_process()
            time.sleep(CHECK_INTERVAL_SECONDS)
    except KeyboardInterrupt:
        print("\n--- 脚本已被用户手动停止 ---")
