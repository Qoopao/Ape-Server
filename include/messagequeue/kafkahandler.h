#ifndef KAFKAHANDLER_H
#define KAFKAHANDLER_H

#include "messagequeue/message_handler.h"

// KafkaHandler 继承自通用 MessageHandler，保持向后兼容
class KafkaHandler : public MessageHandler {
public:
    virtual ~KafkaHandler() = default;
};

#endif
