echo starting server with 10 threads
./server 8080 . 10 50 HPIC > server_output.txt &
echo starting client with 10 threads
./client localhost 8080 10 FIFO /zubat.jpg /index.html > client_output.txt &
echo sleeping for 5 seconds
sleep 5s
echo killing client, then server
pkill client
pkill server
