# Instructions to Compile and Use / Requirements
- `make` then `./out/theo_pricer theo_data.csv underlying_prices.txt` is sufficient
- No third party packages are used
- Output is written to 'result.csv' as specified

# Assumptions
- Assuming that incoming `theo_data.csv` file will follow consistent ordering of elements  
  - i.e. each line is in order of instrument,reference_theo,reference_underlying_price,delta,gamma  
  - Malformed lines are skipped
- Rounding output to 4 digits; example output in provided README shows 4 digits

# Approach

## Part 1: Formula Reduction
- Performing O(1) calculation per average theo, by reducing given formula into:  
  `Theo(U) = RefTheo + Delta*(U - RefU) + 0.5*Gamma*(U - RefU)^2`

- Precomputing constants:  
  `A = RefTheo - Delta*RefU + 0.5*Gamma*RefU^2`  
  `B = Delta - Gamma*RefU`  
  `C = 0.5*Gamma`

- Accordingly precomputing average of the underlying price and average of the squared underlying price 
- Per average theo becomes:  
  `avgTheo = A + B*avgU + C*avgU2`

## Part 2: String Parsing
- `std::strings` are created minimally; string parsing is done using C-style APIs when possible, to avoid `std::string` overhead
- Most parsing logic is encapsulated in 'StringParser'

## Part 3: Producer-Consumer Model and I/O
- SPMC model is used, with 1 thread reading from `theo_data.csv` and worker threads calculating theos using data pushed to queue
- `std::vector` is used instead of `std::queue` to allow for addition and removal of elements to end of the queue, to prevent shifting of elements
- Lines are pushed to queue in batches to reduce worker spinning and contention on queue lock
- Workers write to final file as they process each batch to maximize parallelism, as opposed to pushing to in-memory buffer and then flushing with a final write

## Part 4: Concurrency Management
- Standard mutexes used for SPMC queue access and access to write file
- Condition variables used to prevent worker threads from busy spinning
