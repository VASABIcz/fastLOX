
using namespace std;

/*#undef DEBUG
#undef ERROR
#undef SRC*/

#ifdef SRC
#define LOG_FORMAT stringify("[{}] [{}:{}] ", __FUNCTION__, __FILE__, __LINE__)
#else
#define LOG_FORMAT stringify("[{}] ", __FUNCTION__)
#endif

#ifdef DEBUG
#define LOGDF(msg, ...) LOGD(LOG_FORMAT << stringify(msg, __VA_ARGS__))
#define LOGD(msg) cout << "[DEBUG] " << msg << endl
#else
#define LOGDF(msg, ...)
#define LOGD(msg)
#endif

#ifdef ERROR
#define LOGEF(msg, ...) LOGD(LOG_FORMAT+fomrat(msg, __VA_ARGS__))
#define LOGE(msg) cout << "[ERROR] " << msg << endl
#else
#define LOGEF(msg, ...)
#define LOGE(msg)
#endif