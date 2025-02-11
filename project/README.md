# Description of Work (10 points)

The main design choice I made was implementing both the receiving and send buffers as linked lists of packets. This made the buffers easily mutable and allowed
for simple packet addition, removal, and sequential access with them. The sending of packets was tied to the send buffer, with packets being added each iteration 
if input from stdin was available. Sequence numbers and acknowledgement numbers were also automatically tracked, with sequence numbers just increasing by one
for each packet added to the buffer, and acks tied to the stdout outputs of the receiving buffer. The only exception was the three handshake packets at the start, which
I hardcoded to be added to the send buffers at the start so that I could hang everything until the handshake was completed. Packets only start getting added to the 
send buffers after the handshake is completed (tracked by a variable completed). I also opted to just use MAXWINDOW as the window for every packet, just to keep it 
simple (like was stated on Piazza), but functionality for increasing window sizes could be easily added. 

One of the problems I encountered was incorrectly adding the Parity flag for the checksum. Because I had implemented this before I had finished the retransmission logic and the transmission of dedicated acks, I had originally added the flag check during packet creation. However, it was common that after packets were added to the send buffer, that their acks would have to be changed, and their ack flag would be set. So I had to refactor all this and instead lifted the sending logic into its own function, where I added the checksum calculation right before each packet is sent. 

I also had problems with correctly setting the ack number of the packets sent out, mostly because acks could come in after packets were already added to the send buffer.
This was fixed by just making sure that next queued packet had the ack correctly set when a packet that needed acknowledgement came in. In the case that there was no
queued packet or too many packets had been sent, a dedicated ack packet is sent, instead, as per specs. 

Additionally, I also encountered a problem with setting the sequence numbers for dedicated ack packets and how the client/server should handle them. I had originally
had it use up the current sequence number and tried to have the receiver ignore it by not adding it to the receiving buffer (but still processing the ack), but this 
had problems with reordered packets. I solved this when I noticed that the reference logs just used 0 for the sequence num for these packets, so I just used 0 as well,
which worked.
