#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# keep easy understand and simple to extend.
# same as keras https://github.com/uber/horovod/blob/master/examples/keras_imagenet_resnet50.py 

import tensorflow as tf
import yaml
import sys 

"""
As keep simple and keep scalability, just wrapper the min logic
this just a configure file
"""

tf.logging.set_verbosity(tf.logging.DEBUG)

# modules
sys.path.append('.')

from name_dict import NameDict

from data_set      import cifar10_dataset
from model         import official_model
from learning_rate import PiecewiseLR
from pipeline      import multipipeline

import train_hook

'''
setup:
    1. install nccl2: 
        perfer : NCCL 2.1.0, this issue is no longer present when using CUDA 9,
        https://docs.nvidia.com/deeplearning/sdk/nccl-install-guide/index.html
  
    2. install openmpi3.0:
        wget https://www.open-mpi.org/software/ompi/v3.0/downloads/openmpi-3.0.0.tar.gz && \
        tar zxf openmpi-3.0.0.tar.gz && \
        cd openmpi-3.0.0 && \
       ./configure --enable-orterun-prefix-by-default && \
        make -j $(nproc) all && \
        make install && \
        ldconfig && \
   
    3. install horovod
        HOROVOD_GPU_ALLREDUCE=NCCL pip install --no-cache-dir horovod

    4. some configure:
        # Configure OpenMPI to run good defaults:
        # --bind-to none --map-by slot --mca btl_tcp_if_exclude lo,docker0
        echo "hwloc_base_binding_policy = none" >> /usr/local/etc/openmpi-mca-params.conf && \
        echo "rmaps_base_mapping_policy = slot" >> /usr/local/etc/openmpi-mca-params.conf && \
        echo "btl_tcp_if_exclude = lo,docker0" >> /usr/local/etc/openmpi-mca-params.conf
    
    5. # Set default NCCL parameters
        echo NCCL_DEBUG=INFO >> /etc/nccl.conf && \
        echo NCCL_SOCKET_IFNAME=^docker0 >> /etc/nccl.conf
    6. for nvidia docker 
        --shm-size=1g --ulimit memlock=-1

    


usage:
   mpirun -np 4 \
    -H server1:1,server2:1,server3:1,server4:1 \
    -bind-to none -map-by slot \
    -x NCCL_DEBUG=INFO -x LD_LIBRARY_PATH -x PATH \
    -mca pml ob1 -mca btl ^openib \
    python train.py
    cifar10_train.py [cifar10_train.yaml]

TODO: 
 - save checkpiont by params
'''


def update_param(pipe):
    train_file = './cifar10_train.yaml' if len(sys.argv) == 1 else sys.argv[1]

    print(train_file)
    with open(train_file, 'r') as f:
        param = yaml.load(f)

    param['all_step'] = max(int(param.epoch * param.train_nums / param.minibatch), 10)
    param['batch_size'] = max(int(param.minibatch / pipe.gpu_nums), 1)
    param['minibatch'] = param.batch_size*pipe.gpu_nums

    #this is nessary compute lr with epoch 
    param['train_nums'] = (param.train_nums // param.minibatch) * param.minibatch

    tf.logging.info("Param:" + repr(param))
    return param

def main(argv):
    pipe  = pipeline.Pipeline()

    # 1. config load, as hypter params should define in yaml config
    param = update_param(pipe) 

    # 2. selet a model, dataset, lr, opt, and so on,  as these can be enumeration.
    create_model_func = official_model.Cifar10Model(param.resnet_layer, param.class_num)

    train_set, vaild_set, eval__set = cifar10_dataset(param.data_path, param.batch_size, param.epoch)
    
    global_step = tf.train.get_or_create_global_step()
    lr = PiecewiseLR(param)
    opt = tf.train.MomentumOptimizer(lr, param.momentum)

    #3 set up graph these should not modify
    #3.1 set_up dataset
    device_dataset = pipe.setup_dataset(train_set)

    #3.2 set_up model and loss    
    device_labels, device_predicts = pipe.setup_model(device_dataset, create_model_func)
    
    
    def compute_loss_func(labels, logits):
        loss = tf.losses.sparse_softmax_cross_entropy(logits=logits, labels=labels)
        return tf.reduce_mean(loss)
    device_losses, sum_loss = pipe.setup_loss(device_labels, device_predicts, compute_loss_func, param.weight_decay)
    
    # 
    def accuracy(labels, predicts, topk):
        return tf.reduce_sum(tf.cast(tf.nn.in_top_k(predicts, labels, topk), tf.float32))

    top1 = pipe.setup_reduce(device_labels, device_predicts, lambda x,y:accuracy(x,y, 1), use_mean=True)
    top5 = pipe.setup_reduce(device_labels, device_predicts, lambda x,y:accuracy(x,y, 5), use_mean=True)

    
    #3.3 set_up gradient compute and update
    train_op = pipe.setup_train(device_losses, opt)
    
    
    #TODO add restore hook
    
    hooks = pipe.get_hook() + [
         tf.train.StopAtStepHook(last_step = param.all_step),
         train_hook.SaverHook(param.checkpoint_dir, save_every_n_steps=100),
         train_hook.TrainStateHook(param, lr, sum_loss, 
                                    {'batch_top1': top1, 'batch_top5': top5},
                                   every_steps=100),
    ]
    

    config = tf.ConfigProto(
        allow_soft_placement=True, log_device_placement=False)
    config.gpu_options.allow_growth = True
    #config.gpu_options.per_process_gpu_memory_fraction = 0.4

    
    # start train loop 
    with tf.train.MonitoredTrainingSession(hooks=hooks,
                                           config=config) as mon_sess:
        while not mon_sess.should_stop():
            mon_sess.run([train_op])
            
if __name__ == "__main__":
    tf.app.run()
