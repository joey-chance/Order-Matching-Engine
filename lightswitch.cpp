#include <mutex>
#include <semaphore.h>

class Lightswitch {
    private:
        std::mutex mtx;
        sem_t& ext_sem;
        int counter = 0;
    public:
        Lightswitch(sem_t& external_semaphore): ext_sem(external_semaphore) {};
        void enter();
        void exit();
        int get_counter();  //used to run check counter, in case exit throws and we need to keep exiting to release ext_sem
};

void Lightswitch::enter() {
    std::unique_lock lock(mtx);
    counter++;
    if (counter == 1) sem_wait(&ext_sem);
}

void Lightswitch::exit() {
    std::unique_lock lock(mtx);
    counter--;
    if (counter == 0) sem_post(&ext_sem);
}

int Lightswitch::get_counter() {
    std::unique_lock lock(mtx);
    return counter;
}