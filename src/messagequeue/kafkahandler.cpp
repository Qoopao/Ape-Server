#include "messagequeue/kafkahandler.h"
#include <iostream>

// 默认消息处理器的实现：打印消息（你可以替换成自己的业务逻辑，比如入库、调用接口等）
void DefaultHandler::handle(const std::string& topic, const std::string& msg) {
    // 业务逻辑
}