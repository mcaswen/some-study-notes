#include <mutex>

// 推荐写法
class Singleton 
{
    public:
        // 禁止拷贝和赋值运算符
        Singleton(const Singleton&) = delete;
        Singleton& operator=(const Singleton&) = delete;

        Singleton(Singleton&&) = delete;
        Singleton& operator=(Singleton&&) = delete;

        Singleton& GetInstance()
        {
            static Singleton instance; // C++11之后才支持并推荐的单例模式

            return instance;
        }

    private:
        // 私有构造函数
        Singleton() {}
        ~Singleton() {}

};

// C++11 之前的写法
class SingletonBeforeCpp11 
{
    public:
        // C++11后才支持
        // SingletonBeforeCpp11(const SingletonBeforeCpp11&) = delete;
        // SingletonBeforeCpp11& operator=(const SingletonBeforeCpp11&) = delete;

        static SingletonBeforeCpp11* GetInstance()
        {
            // 用到的时候再创建（懒加载）
            // 但是多线程不安全
            // 加一把锁，一次只允许一个线程通过，
            std::lock_guard<std::mutex> lock(_mutex);

            if (!_instance)
            {
                _instance = new SingletonBeforeCpp11();
            }

            return _instance;
        }

        static SingletonBeforeCpp11* GetInstance()
        {
            // 从语义上保证只会初始化一次
            std::call_once(_onceFlag, InitInstance);
        }

        void InitInstance()
        {
            _instance = new SingletonBeforeCpp11();
        }

        static void Destroy()
        {
            delete _instance;
            _instance = nullptr;
        }

    private:
        static std::mutex _mutex;

        static std::once_flag _onceFlag;

        static SingletonBeforeCpp11* _instance;

        // 私有构造函数
        SingletonBeforeCpp11() {}
        ~SingletonBeforeCpp11() {}
            
        SingletonBeforeCpp11(const SingletonBeforeCpp11&) {}
        SingletonBeforeCpp11& operator=(const SingletonBeforeCpp11&) {}
        
        // static SingletonBeforeCpp11 _instance = nullptr; // 无法初始化

};

class SingletonDoubleChecked 
{
    public:
        static SingletonDoubleChecked* GetInstance() 
        {
            // 第一次检查：避免每次调用都加锁
            if (_instance == nullptr) 
            {
                // 加锁确保线程安全
                pthread_mutex_lock(&_mutex);
                
                // 第二次检查：防止多个线程同时通过第一次检查
                if (_instance == nullptr) {
                    _instance = new SingletonDoubleChecked();
                }
                
                pthread_mutex_unlock(&_mutex);
            }
            return _instance;
        }

        // 手动清理资源
        static void Destroy() {
            pthread_mutex_lock(&_mutex);
            
            if (_instance != nullptr) {
                delete _instance;
                _instance = nullptr;
            }
            
            pthread_mutex_unlock(&_mutex);
            pthread_mutex_destroy(&_mutex);
        }

    private:
        static SingletonDoubleChecked* _instance;
        static pthread_mutex_t _mutex;
    
        // 私有构造函数
        SingletonDoubleChecked() {}
        ~SingletonDoubleChecked() {}
        
        // 防止拷贝
        SingletonDoubleChecked(const SingletonDoubleChecked&);
        SingletonDoubleChecked& operator=(const SingletonDoubleChecked&);
};
