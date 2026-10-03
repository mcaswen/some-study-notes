#include "1.Singleton.h"

SingletonBeforeCpp11* SingletonBeforeCpp11::_instance = new SingletonBeforeCpp11(); // 定义或初始化是允许的，私有属性只限制读写/访问
std::mutex SingletonBeforeCpp11::_mutex;
std::once_flag SingletonBeforeCpp11::_onceFlag;

//在程序生命周期结束时
// SingletonBeforeCpp11::Destroy();

// 双重检查锁定：静态成员初始化
SingletonDoubleChecked* SingletonDoubleChecked::_instance = nullptr;
pthread_mutex_t SingletonDoubleChecked::_mutex = PTHREAD_MUTEX_INITIALIZER;