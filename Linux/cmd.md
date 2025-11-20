**列出当前占用内存前10的线程**

```bash
# 显示前 11 行（因为第 1 行是标题栏，所以实际显示的是前 10 个进程）。
ps aux --sort=-%mem | head -n 11 
```



**寻找一个文件的位置**

```go
# 在当前目录及其子目录下查找
find . -name "xxx.py"
# 在你用户的主目录下查找
find ~ -name "xxx.py"
# 全盘扫描
sudo find / -name "xxx.py" 2>/dev/null
# 根据PID直接查看这个进程的“工作目录”
pwdx 1375137
```



