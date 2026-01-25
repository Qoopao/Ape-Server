#include <cstdint>
#include <vector>
#include "messagequeue/kafkaproducer.h"
#include "messagequeue/kafkaconsumer.h"
#include <thread>
#include <gtest/gtest.h>

void testMQ();

void produceMsgTask(){
    // Kafka生产者
    std::string brokerList = "localhost:29092,localhost:39092,localhost:49092";
    std::string topicName = "test_topic";
    KafkaProducer producer(brokerList, topicName);
    std::string key_1 = "test_key_1";
    std::string key_2 = "test_key_2";
    std::string key_3 = "test_key_3";


    int32_t counter = 0;
    while(1){
        std::string payload = "test_payload" + std::to_string(counter);
        std::string key = "key_"+std::to_string(counter%3);
        producer.Deliver(key, (void*)payload.c_str(), payload.size());
        counter++;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void testMQ(){
    // 启动生产者线程
    std::thread producerThread(produceMsgTask);
    producerThread.detach();


    // Kafka消费者
    std::string brokerList = "localhost:29092,localhost:39092,localhost:49092";
    std::string topicName = "test_topic";
    KafkaConsumer consumer(brokerList, "test_group", std::vector<std::string>({topicName}));
    consumer.start();

}

