### shouter
- There is a place where neither network  nor telephone, even no electric, they communicate by shouter, loudly shouter
- A parallel interface with multi node, unlike MPI, shouter use global step to sequence message, So its easy to sync paramter in Synchronous SGD training with out coordinator
- this is **imitate(copy)** from [uber/horovod](https://github.com/uber/horovod), all rights belong to uber/horovod Now.

### motivation
- As Synchronous SGD distribute training, the key problem is how to sync grad aross multi worker.
  this work focus mutli machine synchronization data, Two optimize:
    1. The bandwidth Utilization
    2. The lantency when reduced 

### the GPU topology 
```
              |           |
             [root] <-> [root]
              |     \ /   |
              |     / \   |
            [ leaf ]   [ leaf ]     ---- (up  grid)
            | | | |    | | | |
            N N N N    (N N N N)   ----- (sub grid)
```
- As many data centor or hpc use FatTree as topo, although they have multi-link redundancy
  There are many allreduce algorithm, See also : [allreduce_halving_doubling](https://github.com/facebookincubator/gloo/blob/master/docs/algorithms.md#allreduce_halving_doubling)
- Now the better strategy hierarchy reduce:
    1. reduce in (sub grid): local rank 0 as master gatter and sum, assume Rank0 has to P2P to other
    2. reduce in (up grid):  local rank 0 as master gatter and sum
    3. recursion to root, then backwise to sub grid


####  how to used bandwidth Utilization:
- TCP: as keep many connection send same time
- UDP: use UDP, but need to implement crc and resend.

#### how to keep low lantency
- seastar&DPDK: use seastar tcp/ip stack as we can extends it later althougth its black hole future&promise 
  -  netmap, PF_RING DNA has little github to copy.
  - https://github.com/F-Stack/f-stack & http://www.f-stack.org/
  - seastar: https://github.com/scylladb/seastar
  - ans: https://github.com/ansyun/dpdk-ans

- RDMA: 
  - DPDK vs RDMA : see https://www.jianshu.com/p/09b4b756b833?utm_campaign
  - softRoce : need kernel module and lib 
  - RoceV2 : need configure driver

### timeline

``` 
    reduceop  |  shared_map | shared_queue |  MessagerA      
TFOP ->|
       | -tensor- push ->|
       |       ---push------------>|          ^ ->Loop 
       |                                      ^   v -> send_tensor-> B 
       |                                      ^   | -> send_tensor-> C
       |                                      ^   | ...
       |  <---------------------notify--------^ <-v  on wait B,C ..
 done<-|
``` 

### op & done design
- shouter.init(iplist, ports, channel_num=10): 
   - setup every channel
   - build local master
   - setup local shared memery

- shouter.brocast(tensor): send tensor to master 
- shouter.allreduce(tensor): recevie tensor from master


### architecture
```
    Node A                                               Node B    
TF Operation   ------(allreduce, brocast)------->     TF Operation  
        |                                                 |                    
        V                                                 V
  coordinator -(step,rank,Tensor(name,shape,buffer)->  coordinator
    /   |                                                 |   \
 Proxy  |             [SeaStarTcpProxy]                   |   Proxy
    \   v                                                 V   /
　 channel -write->Message(step,TensorID,buffer)-read->  channel
                   [SeaStarTcpChannel] 
```

