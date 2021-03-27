# httpserver.c (Multithreading edition!)

### Jay Montoya
### 1742317 | jaanmont @ucsc.edu

#### Instructions for building "httpserver":
* make: builds the project 
* make clean: removes built artifacts except for the final executable
* make spotless: removes all built artifacts including the final executable

#### Run httpserver in the following format: 
#### "./httpserver [port number] -N {num_threads} -l {log_file}"
* *port number* must refer to a valid port that the server will listen on
* "-N" is an optional flag that specifies the number of threads to be used. 
* "-l" is an optional flag specifying the name of the log file to be used.
* Optional flags may appear in any order according to the assignment specification.

#### Known bugs (PLEASE READ): 
- A very overladed server with multiple PUT requests to the same file may result 
in undefined behavior due to the logging approach taken. See design doc section 
on logging for more.
- If HTTP request tokens are not seperated by exactly one space where 
applicable, the behavior of the server is undefined. This was not fixed since 
the TA's said it was OK in the FAQ.

