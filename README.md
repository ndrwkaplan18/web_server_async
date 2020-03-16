# web_server_async
Section 1:
Group members: Andrew Kaplan (amkapla2), CJ Wiesenfeld(cjwiesen).
Division of work: Andrew made the server and client multithreaded and deamonized the server. CJ implemented the scheduling policies and statistics. In other words, Andrew worked on stages 1,4 & 5, whilst CJ worked on 2 & 3. That being said, both partners assisted the other with brain storming, design and debugging.

Section 2, Design Overview:
Server: Upon recieeing requests from the client, the server sends it to the thread pool buffer, where it sits until the master thread removes it, determines the filetype, and assigns a thread to handle the job. Depending on the scheduling policy delineated at runtime, the removal that takes place will either be FIFO, HPHC, or HPIC. 
Once removed, the the server will insure the request is valid with the web() method, and will log and display it accordingly. 

Section 3, Complete Specs:
Regarding Ambiguities, for the ANY command, we simply called FIFO.

Section 4, Known bugs or problems: 
Every once in a while it seems the client output stream writes to the input stream and causes the response to be a 403

Section 5, Testing:
To test for concurrency in the server and the client, we run the server as a single thread with each scheduling protocall so it is easy to verify.
To ensure the full functioning of the scheduling policies, we call the server with said policy and send a variety of requests. For FIFO, the requests would be handled in a first in first out bases regardless of request type, and as such, the usage statistics for each request, as well as each thread will show that that was so. For HPHC, the requests would be handled in order of any text first, and the relevant statistics will reflect that as well. For HPIC it would be the same but reversed.  
