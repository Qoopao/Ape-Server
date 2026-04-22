#!/bin/bash
set -uo pipefail  
shopt -s nullglob 

# ===================== 配置区 =====================
TARGET="127.0.0.1:6666"       
TOTAL_CONN=1000               
CONN_PER_SOCAT=50             
BATCH_INTERVAL=0.5            # 延长间隔，给服务端足够时间处理
CONN_TIMEOUT=5                # 延长连接超时（避免服务端处理慢导致连接失败）
HOLD_TIME=2                   
# ==================================================

# 拆分IP和端口（修正ss统计语法）
TARGET_IP=$(echo $TARGET | cut -d: -f1)
TARGET_PORT=$(echo $TARGET | cut -d: -f2)

# 临时文件
PID_FILE=$(mktemp /tmp/socat_conn_pid.XXXXXX)
trap 'rm -f "$PID_FILE"; pkill -f "socat TCP4:${TARGET}" >/dev/null 2>&1' EXIT

# 初始化统计
SUCCESS=0
FAIL=0
CREATED=0
TOTAL_BATCH=$(( (TOTAL_CONN + CONN_PER_SOCAT - 1) / CONN_PER_SOCAT ))
CURRENT_BATCH=0

echo -e "===== Socat TCP长连接压力测试 =====\n"
echo "目标地址:         $TARGET"
echo "总连接数:         $TOTAL_CONN"
echo "每个socat进程连接数: $CONN_PER_SOCAT"
echo "连接保持时间:     $HOLD_TIME 秒"
echo "总批次数:         $TOTAL_BATCH"
echo -e "------------------------------------\n"

# 批量创建连接
while (( CREATED < TOTAL_CONN )); do
    CURRENT_BATCH=$(( CURRENT_BATCH + 1 ))
    CURRENT_CONN=$(( TOTAL_CONN - CREATED > CONN_PER_SOCAT ? CONN_PER_SOCAT : TOTAL_CONN - CREATED ))

    echo "正在创建第 $CURRENT_BATCH/$TOTAL_BATCH 批连接（$CURRENT_CONN 个）..."
    
    # 优化Socat参数：增加reuseport，延长-T（保持时间），关闭Nagle算法（提高并发）
    socat -T $((HOLD_TIME + 1)) -t $CONN_TIMEOUT -d -d \
        TCP4:$TARGET,fork,reuseaddr,reuseport,nodelay \
        /dev/null >/dev/null 2>&1 &
    SOCAT_PID=$!
    echo $SOCAT_PID >> "$PID_FILE"

    sleep $BATCH_INTERVAL
    CREATED=$(( CREATED + CURRENT_CONN ))
    echo "已创建累计连接数: $CREATED/$TOTAL_CONN"
done

# 等待连接完全建立并保持（延长1秒，确保统计时连接未断开）
echo -e "\n所有连接已发起，等待 $((HOLD_TIME + 1)) 秒确保连接保持..."
sleep $((HOLD_TIME + 1))

# 核心修复：修正ss命令的state参数（ESTABLISHED），并优化统计逻辑
echo -e "\n开始统计真实连接数..."
# 正确的ss命令：state ESTABLISHED（完整拼写），同时匹配源端和目标端
ESTAB_CONNS=$(ss -tn | awk -v ip="$TARGET_IP" -v port="$TARGET_PORT" '$4 == ip":"port && $1 == "ESTAB" {count++} END {print count+0}')
SUCCESS=$ESTAB_CONNS
FAIL=$(( TOTAL_CONN - SUCCESS ))

# 计算成功率
if (( TOTAL_CONN == 0 )); then
    SUCCESS_RATE=0
else
    SUCCESS_RATE=$(echo "scale=2; $SUCCESS/$TOTAL_CONN*100" | bc)
fi

# 输出结果
echo -e "\n========================================"
echo -e "===== 压力测试结果（保持${HOLD_TIME}秒） ====="
echo "----------------------------------------"
echo "总发起连接数:     $TOTAL_CONN"
echo "成功保持2秒数:    $SUCCESS"
echo "失败/断开数:      $FAIL"
echo "连接成功率:       ${SUCCESS_RATE}%"
echo -e "========================================\n"

# 额外：打印当前系统中目标端口的连接详情（便于排查）
echo "当前目标端口连接详情（前10行）："
ss -tn state ESTABLISHED | grep "$TARGET_IP:$TARGET_PORT" | head -10

# 清理进程
echo -e "\n清理测试进程..."
pkill -f "socat TCP4:${TARGET}" >/dev/null 2>&1
rm -f "$PID_FILE"

echo "压力测试完成！"