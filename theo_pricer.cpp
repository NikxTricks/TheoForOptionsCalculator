#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <array>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <algorithm>

class StringParser {
    public:
        // parse double using strtod to avoid overhead of creating new std::strings for use with stod
        [[nodiscard]] static inline bool parseDouble(const char *s, const char *e, double &val)
        {
            char buff[48];

            size_t len = (size_t)(e - s);
            if (len >= sizeof(buff)) [[unlikely]] {
                len = sizeof(buff) - 1;
            }

            std::memcpy(buff, s, len);

            // strtod requires null terminator
            buff[len] = '\0';

            char *tail = nullptr;
            val = strtod(buff, &tail);

            // verify no garbage was included in parsing
            while (*tail && std::isspace((unsigned char)(*tail))) ++tail;
            return *tail == '\0';
        }

        [[nodiscard]] static inline bool lineToInstrument(const std::string &line, std::string &name, double &A, double &B, double &C) {
            size_t comma1 = line.find(','); if (comma1 == std::string::npos) [[unlikely]] return false; 
            size_t comma2 = line.find(',', comma1 + 1); if (comma2 == std::string::npos) [[unlikely]] return false; 
            size_t comma3 = line.find(',', comma2 + 1); if (comma3 == std::string::npos) [[unlikely]] return false; 
            size_t comma4 = line.find(',', comma3 + 1); if (comma4 == std::string::npos) [[unlikely]] return false; 

            const char *s = line.data();
            const char *e = s + line.size();

            // prepare strings for parsing
            const char *refTheoS = s + comma1 + 1; const char *refTheoE = s + comma2;
            const char *refUpS = s + comma2 + 1; const char *refUpE = s + comma3;
            const char *deltaS = s + comma3 + 1; const char *deltaE = s + comma4;
            const char *gammaS = s + comma4 + 1; const char *gammaE = e;

            double refTheo, refUp, delta, gamma;
            if (!parseDouble(refTheoS, refTheoE, refTheo)) [[unlikely]] return false;
            if (!parseDouble(refUpS, refUpE, refUp)) [[unlikely]] return false;
            if (!parseDouble(deltaS, deltaE, delta)) [[unlikely]] return false;
            if (!parseDouble(gammaS, gammaE, gamma)) [[unlikely]] return false;

            name.assign(line.data(), comma1);

            // calculate constants for O(1) calculation
            A = refTheo - (delta * refUp) + 0.5 * gamma * (refUp * refUp);
            B = delta - gamma * refUp;
            C = 0.5 * gamma;

            return true;
        }
};
class Theo
{
    static constexpr int MAX_NUM_INSTRUMENTS = 32000;
    static constexpr int BATCH_SIZE = 500;

    struct InstrumentData
    {
        std::string name_;
        double A_, B_, C_;

        InstrumentData() = default;

        // ctor for c++11/17 compatibility with emplace operations
        InstrumentData(std::string name, double A, double B, double C) : name_(std::move(name)), A_(A),
                                                                         B_(B), C_(C) {}
    };

    // batch elements, so each worker threads spends time working and less time spinning on locks
    struct Batch
    {
        std::array<InstrumentData, BATCH_SIZE> data;
        int size = 0;
    };

    std::filesystem::path theoInputPath_;
    std::filesystem::path underlierInputPath_;

    // lock for writing / reading SPMC queue
    std::mutex instrumentsLock;

    // cond var to park worker threads
    std::condition_variable instrumentsEmpty;

    // SPMC queue
    std::vector<Batch> instruments;
    bool done = false;

    // lock for writing to output file
    std::mutex writeLock;

    // running values of underlying prices for O(1) theo calculation
    double runningAverageUp;
    double runningAverageUpSquared;

public:
    Theo(std::filesystem::path theoInputPath, std::filesystem::path underlierInputPath) : theoInputPath_(theoInputPath), underlierInputPath_(underlierInputPath), runningAverageUp(0.0), runningAverageUpSquared(0.0)  {
        instruments.reserve(MAX_NUM_INSTRUMENTS / BATCH_SIZE);
    }

    [[nodiscard]] int calcTheos() {
        bool successful = false;

        std::thread underlyingPricesReader(&Theo::readUnderlyingPrices, this, std::ref(successful));
        underlyingPricesReader.join();
        if (!successful) [[unlikely]] {
            return 1;
        }

        std::ofstream fileToWrite("result.csv");
        if (!fileToWrite.is_open()) [[unlikely]] {
            return 1;
        }

        // write header and set io configs for writing
        fileToWrite << "instrument,average_theo\n";

        std::thread theoDataReader(&Theo::readTheoData, this, std::ref(successful));

        // spec says 16 cores, but use this check just in case
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const int numRemainingThreads = (int) ((hw > 1) ? hw - 1 : 1);
        std::vector<std::thread> workerThreads;
        workerThreads.reserve(numRemainingThreads);
        for (int i = 0; i < numRemainingThreads; i++)
        {
            workerThreads.emplace_back(&Theo::processBatch, this, std::ref(fileToWrite));
        }

        theoDataReader.join();
        for (int i = 0; i < numRemainingThreads; i++)
        {
            workerThreads[i].join();
        }

        if (!successful) [[unlikely]] {
            return 1;
        }

        return 0;
    }

  private:
    void pushBatch(Batch &&incoming) {
        {
            std::scoped_lock lk_(instrumentsLock);

            instruments.push_back(std::move(incoming));
        }

        instrumentsEmpty.notify_one();
    }

    [[nodiscard]] bool popBatch(Batch &out) {
        std::unique_lock<std::mutex> lk_(instrumentsLock);

        if (instruments.empty()) {
            // predicate for parking
            instrumentsEmpty.wait(lk_, [&]() -> bool {
                return !instruments.empty() || done;
            });
        }
        if (instruments.empty()) [[unlikely]] {
            return false;
        }

        // read from back of queue to avoid shifting elements
        out = std::move(instruments.back());
        instruments.pop_back();

        return true;
    }

    void doneReading() {
        {
            std::scoped_lock lk_(instrumentsLock);
            
            done = true;
        }

        instrumentsEmpty.notify_all();
    }

    void readTheoData(bool &success) {
        success = false;
        std::ifstream theoFile(theoInputPath_);
        if (!theoFile.is_open()) [[unlikely]] {
            return;
        }

        std::string line;

        Batch currentBatch;
        std::string name;
        double A;
        double B;
        double C;

        bool first = true;
        while (getline(theoFile, line)) {
            if (first && line.find("INSTRUMENTS") != std::string::npos) {
                first = false;
                continue;
            }
            if (StringParser::lineToInstrument(line, name, A, B, C)) {
                InstrumentData &instrument = currentBatch.data[currentBatch.size];
                instrument.name_ = std::move(name);
                instrument.A_ = A;
                instrument.B_ = B;
                instrument.C_ = C; 

                if (++currentBatch.size == BATCH_SIZE) {
                    pushBatch(std::move(currentBatch));
                    currentBatch = Batch{};
                }
            }
        }

        // push lingering batch
        if (currentBatch.size > 0) {
            pushBatch(std::move(currentBatch));
        }

        // wake up all parked threads
        doneReading();

        success = true;
    }

    void readUnderlyingPrices(bool &success) {
        success = false;
        int count = 0;
        double underlyingPrice;

        std::ifstream underlyingPricesFile(underlierInputPath_);
        if (!underlyingPricesFile.is_open()) [[unlikely]] {
            return;
        }

        // calculate running averages of underlying prices for O(1) theo calculations
        while (underlyingPricesFile >> underlyingPrice)
        {
            count++;
            runningAverageUp = runningAverageUp + (underlyingPrice - runningAverageUp) / count;
            runningAverageUpSquared = runningAverageUpSquared + ((underlyingPrice * underlyingPrice) - runningAverageUpSquared) / count;
        }
        if (count == 0) [[unlikely]] {
            return;
        }

        success = true;
    }

    void processBatch(std::ofstream &fileToWrite) {
        // write all results to same std::string to avoid overhead of creating multiple std::strings
        std::string buff;
        buff.reserve(BATCH_SIZE * 64);

        const double avg = runningAverageUp;
        const double avgS = runningAverageUpSquared;

        Batch currentBatch;
        while (popBatch(currentBatch)) {
            for (int i = 0; i < currentBatch.size; i++)
            {
                InstrumentData &currentInstrument = currentBatch.data[i];

                double result = currentInstrument.A_ + avg * currentInstrument.B_ + avgS * currentInstrument.C_;
                buff.append(currentInstrument.name_);
                buff.append(",");

                // round and truncate to 4 decimals
                char num[32];
                auto [p, ec] = std::to_chars(num, num + sizeof num, result, std::chars_format::fixed, 4);

                buff.append(num, p);
                buff.append("\n");
            }

            std::scoped_lock lk_(writeLock);
            fileToWrite << buff;

            buff.clear();
        }
    }
};

using namespace std::chrono;

size_t microsecondsBetween(system_clock::time_point startTime,
                           system_clock::time_point endTime) {
  return std::chrono::duration_cast<std::chrono::microseconds>(endTime -
                                                               startTime)
      .count();
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cout << "Usage: theo_pricer <theo_data.csv> <underlying_prices.txt>"
              << std::endl;
    return -1;
  }
  const char *theoInputPath = argv[1];
  const char *underlierInputPath = argv[2];
  auto startTime = system_clock::now();

  Theo t(theoInputPath, underlierInputPath);

  try {
    std::cout << t.calcTheos() << "\n";
  }
  catch (const std::runtime_error &e) {
    std::cerr << e.what() << "\n";
  }

  auto endTime = system_clock::now();

  std::cout << "Load time mics: " << microsecondsBetween(startTime, endTime)
            << std::endl;
  return 0;
}
