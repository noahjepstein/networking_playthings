#### COMP 112 ASSIGNMENT 3: Reliable File Transfer Over UDP

Question: when you read into a buffer, does it increment the pointer?

Need to deal with:
- reading in files and breaking them into chunks of 512
- dividing those chunks into transmission windows
- deal with arbitrary window size

__READS__
- can be an ack
- can be an RRQ
- could read RRQ size and it's just an ack
