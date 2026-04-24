#ifndef HR_RECEIVER_H
#define HR_RECEIVER_H

#include <pthread.h>

class HrReceiver {
public:
    explicit HrReceiver(const char* udsPath = "/tmp/polar_hr.sock");
    ~HrReceiver();

    bool start();
    void stop();
    bool getLatest(int& hr, int& rr) const;

private:
    static void* readerThread(void* arg);
    bool openSocket();
    void closeSocket();

    const char* udsPath_;
    int udsSockFd_;
    pthread_t thread_;
    mutable pthread_mutex_t lock_;
    bool running_;
    int latestHr_;
    int latestRr_;
    bool hasValue_;
};

#endif // HR_RECEIVER_H
