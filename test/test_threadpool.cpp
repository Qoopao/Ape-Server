#include <util/threadpool.h>
#include <spdlog/spdlog.h>
#include <iostream>

int calc_task(int a, int b) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int res = a * b + a - b; // 简单计算逻辑
    std::cout << "【计算任务】线程ID: " << std::this_thread::get_id() << ", " << a << "*" << b << "+" << a << "-" << b << " = " << res << std::endl;    
    return res;
}
void print_task(int task_id, const std::string& content) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 模拟任务耗时
    std::cout << "【打印任务" << task_id << "】线程ID: " << std::this_thread::get_id() << ", 内容: " << content << std::endl;
}

void test_threadpool() {

    try {
        // 1. 获取线程池实例（初始化4个工作线程）
        spdlog::info("===== 初始化线程池（4个工作线程） =====");
        ThreadPool& pool = ThreadPool::get_thread_pool_instance();

        // 2. 提交批量无返回值任务（测试并发执行）
        spdlog::info("\n===== 提交6个打印任务 =====");
        for (int i = 1; i <= 6; ++i) {
            std::string content = "线程池并发测试任务";
            pool.put_task(print_task, i, content);
        }

        // 3. 提交有返回值任务（测试future获取结果）
        spdlog::info("\n===== 提交3个计算任务 =====");
        auto f1 = pool.put_task(calc_task, 10, 5);
        auto f2 = pool.put_task(calc_task, 20, 8);
        auto f3 = pool.put_task(calc_task, 15, 7);

        // 4. 获取并打印计算任务结果（get()会阻塞直到任务完成）
        spdlog::info("\n===== 获取计算任务结果 =====");
        spdlog::info("计算任务1结果: {}", f1.get());
        spdlog::info("计算任务2结果: {}", f2.get());
        spdlog::info("计算任务3结果: {}", f3.get());

        // 5. 演示调整线程池大小（TODO实现后可验证效果）
        // spdlog::info("\n===== 调整线程池大小为8 =====");
        // pool.resize(8);

        // 6. 等待所有任务执行完毕（模拟主线程业务逻辑）
        spdlog::info("\n===== 等待所有任务执行完成 =====");
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 7. 停止线程池（析构时也会自动停止，主动调用更优雅）
        spdlog::info("\n===== 停止线程池 =====");
        pool.stop();

        spdlog::info("\n===== 线程池测试完成 =====");
    }
    catch (const std::exception& e) {
        spdlog::error("测试异常: {}", e.what());
    }

    // 清理日志
    spdlog::shutdown();
}