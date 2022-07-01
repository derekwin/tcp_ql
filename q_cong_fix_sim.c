#include <linux/module.h>
#include <net/tcp.h>
#include <linux/math64.h>

#define numOfState	1

#define	state0_max	100	// throughput

#define	Q_CONG_SCALE	1024

#define	numOfAction	4

#define epsilon 8   // Explore parameters 0~9 <= epsilon

#define	sizeOfMatrix 	state0_max * numOfAction

static const u32 probertt_interval_msec = 10000;
static const u32 training_interval_msec = 100;
static const u32 max_probertt_duration_msecs = 200;
static const u32 estimate_min_rtt_cwnd = 4;

static const char procname[] = "tcpql";

static const int learning_rate = 512;
static const int discount_factor = 12; 

enum action{
	CWND_UP_30,
	CWND_UP_1,
	CWND_DOWN,
    CWND_NOTHING,
};

enum q_cong_mode{
	NOTHING,
	TRAINING,
	ESTIMATE_MIN_RTT,
	STARTUP,
};

typedef struct{
	u8  enabled;
	int mat[sizeOfMatrix];	
	u8 row[numOfState];
	u8 col;
}Matrix; 

static Matrix matrix;

struct Q_cong{
	u64	alpha;
	bool	forced_update;
	
	u32	mode:3,
		exited:1,
		unused:28;
	u32 	last_sequence; 
	u32	estimated_throughput;
	u32 pre_throughput;
	u32	last_update_stamp;
	u32	last_packet_loss;
	u32 	retransmit_during_interval; 

	u32	last_probertt_stamp;
	u32 	min_rtt_us; 
	u32	prop_rtt_us;
	u32	prior_cwnd;

	u32	current_state[numOfState];
	u32	prev_state[numOfState];
	u32 	action; 
};


static int matrix_init = 0;
static void createMatrix(Matrix *m, u8 *row, u8 col){
	u32 i;
	
	if (!m)
		return;

	m->col = col; 

	for(i=0; i<numOfState; i++)
		*(m->row+i) = *(row+i);

	// use matrix repeatedly
	if(matrix_init == 0){
		for(i=0; i<sizeOfMatrix; i++)
			*(m->mat + i) = 0;
		matrix_init = 1;
	}

	m -> enabled = 1; 
}

static void eraseMatrix(Matrix *m){
	u8 i=0; 
	if (!m)
		return;

	for(i=0; i<numOfState; i++)
		*(m->row + i) = 0; 

	m -> col = 0; 
	m -> enabled = 0; 
}

static void setMatValue(Matrix *m, u8 row1, u8 col, int v){
	u32 index = 0; 
	if (!m)
		return;
	// row1 * m->row[1] * m->row[2] + row2 * m->row[2] + row3
	index = m->col * (row1 ) + col;
	*(m -> mat + index) = v;
}

static int getMatValue(Matrix *m, u8 row1,  u8 col){
	u32 index = 0; 
	if (!m)
		return -1; 

	index = m->col * (row1 ) + col;
	
	return *(m -> mat + index);
}

static u32 q_cong_ssthresh(struct sock *sk){
	return TCP_INFINITE_SSTHRESH;
}

static u32 epsilon_expore(u32 max_index){
	u32 rand;
	u32 rand2;
	u32 random_value;
	get_random_bytes(&rand, sizeof(rand));
	random_value = (rand%10); // 0~9
	if(random_value <= epsilon)
		return max_index;
	get_random_bytes(&rand2, sizeof(rand2));
	return rand2%numOfAction;
}

static u32 getAction(struct sock *sk, const struct rate_sample *rs){
	struct Q_cong *qc = inet_csk_ca(sk);

	u32 Q[numOfAction];
	u8 i;
	u8 is_equal = 1;
	u32 max_index = 0; 
	u32 max_tmp = 0 ;
	u32 rand;	

	for(i=0; i<numOfAction; i++){
		Q[i] = getMatValue(&matrix, qc -> current_state[0],i);
	}

	max_tmp = Q[0];
	for(i=0; i<numOfAction; i++){
		if(max_tmp == Q[i]){
			max_index = i;
			continue;
		}
		is_equal = 0;
		if(max_tmp < Q[i]){
			max_tmp = Q[i];
			max_index = i;
		}
	}
	
	if(is_equal){
		get_random_bytes(&rand, sizeof(rand));
		max_index = (rand%numOfAction);
	}

	// return epsilon_expore(max_index);
	return max_index;
}

static int getRewardFromEnvironment(struct sock *sk, const struct rate_sample *rs){
	//struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);
	u32 retransmit_division_factor; 
    int result;

	retransmit_division_factor = qc -> retransmit_during_interval + 1;
	if(retransmit_division_factor == 0 || rs->rtt_us == 0)
		return 0;
	
	result = qc -> estimated_throughput - qc -> pre_throughput;
	
	printk(KERN_INFO "reward : %d", result);
	
	return result;
}

static void executeAction(struct sock *sk, const struct rate_sample *rs){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);

	switch(qc -> action){
		case CWND_UP_30:
			tp -> snd_cwnd  = tp->snd_cwnd + 30/ (tp->snd_cwnd);
			break;

		case CWND_UP_1:
			tp -> snd_cwnd  = tp->snd_cwnd + 1;
			break;

		case CWND_DOWN:
			tp -> snd_cwnd = tp->snd_cwnd - (tp->snd_cwnd >> 1);
			break; 

		default : 
			break;
	}
}

static u32 q_cong_undo_cwnd(struct sock* sk){
	struct tcp_sock *tp = tcp_sk(sk);
	
	// printk(KERN_INFO "undo congestion control");
	return max(tp->snd_cwnd, tp->prior_cwnd);
}

static void calc_retransmit_during_interval(struct sock* sk){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);

	qc -> retransmit_during_interval = (tp -> total_retrans - qc -> last_packet_loss) * training_interval_msec / jiffies_to_msecs(tcp_jiffies32 - qc -> last_update_stamp);
	qc -> last_packet_loss = tp -> total_retrans; 
}

static void calc_throughput(struct sock *sk){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);

	u32 segout_for_interval;
	
	segout_for_interval = (tp -> segs_out - qc ->last_sequence) * tp ->mss_cache; 

	qc -> pre_throughput = qc -> estimated_throughput;
	qc -> estimated_throughput = segout_for_interval * 8 / jiffies_to_msecs(tcp_jiffies32 - qc -> last_update_stamp); 

	qc -> last_sequence = tp -> segs_out;
}

static void update_state(struct sock *sk, const struct rate_sample *rs){
	struct Q_cong *qc = inet_csk_ca(sk);
    int current_rtt;
	u8 i; 

	for (i=0; i<numOfState; i++)
		qc -> prev_state[i] = qc -> current_state[i];
	
	qc -> current_state[0] = qc -> estimated_throughput>>9;	// test 0~100M
	// qc -> current_state[1] = current_rtt >> 13;	// 0~100ms  8ms/pre

	for(i=0; i<numOfState; i++){
		if(qc -> current_state[i] < 0)
			qc -> current_state[i] = 0;

		else if (qc -> current_state[i] > 99)
			qc -> current_state[i] = 99; 
	}
}

static void update_Qtable(struct sock *sk, const struct rate_sample *rs){
	struct Q_cong *qc = inet_csk_ca(sk);

	int thisQ[numOfAction]; 
	int newQ[numOfAction];
	u8 i;
	int updated_Qvalue;
	int max_tmp; 
	// printk(KERN_INFO "---------");
	for(i=0; i<numOfAction; i++){
		thisQ[i] = getMatValue(&matrix, qc->prev_state[0], i);
		newQ[i] = getMatValue(&matrix, qc->current_state[0], i);
		// printk(KERN_INFO "i: %u, this Q %d, newQ: %d", i ,thisQ[i] ,newQ[i]);
	}

	max_tmp = newQ[0];
	for(i=0; i<numOfAction; i++){
		if(max_tmp < newQ[i])
			max_tmp = newQ[i]; 
	}
	// printk(KERN_INFO "max_tmp: %d", max_tmp);
	updated_Qvalue = ((Q_CONG_SCALE-learning_rate)*thisQ[qc ->action] +
			(learning_rate * (getRewardFromEnvironment(sk,rs) + ((discount_factor * max_tmp)>>4))))>>10;

	if(updated_Qvalue == 0){
		qc -> exited = 1; 
		return;
	}
	// printk(KERN_INFO "preQ %d, updated_Qvalue: %d", thisQ[qc ->action] ,updated_Qvalue);
	setMatValue(&matrix, qc->prev_state[0], qc->action, updated_Qvalue);
}

static void training(struct sock *sk, const struct rate_sample *rs){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);

	u32 training_timer_expired = after(tcp_jiffies32, qc -> last_update_stamp + msecs_to_jiffies(training_interval_msec)); 

	if(training_timer_expired && qc -> mode == NOTHING){
		// printk(KERN_INFO "update count: %d", count);
		if (qc -> action == 0xffffffff)
			goto execute;

		calc_throughput(sk);
		update_state(sk,rs);
		calc_retransmit_during_interval(sk);

		if (qc -> exited == 1){
			tp -> snd_cwnd = TCP_INIT_CWND; 
			qc -> exited = 0; 
			return; 
		}

		update_Qtable(sk,rs);
execute:
		// printk(KERN_INFO "execute Action: %u", qc -> action);
		qc -> action = getAction(sk,rs);
		executeAction(sk, rs);
		qc -> last_update_stamp = tcp_jiffies32; 
	}
}

static void update_min_rtt(struct sock *sk, const struct rate_sample* rs){
	struct tcp_sock *tp = tcp_sk(sk);
	struct Q_cong *qc = inet_csk_ca(sk);
	u32 estimate_rtt_expired; 

	u32 update_filter_expired = after(tcp_jiffies32, 
			qc -> last_probertt_stamp + msecs_to_jiffies(probertt_interval_msec));

	if (rs -> rtt_us > 0){	
		if (rs -> rtt_us < qc -> min_rtt_us){
			qc -> min_rtt_us = rs -> rtt_us;
			qc -> last_probertt_stamp = tcp_jiffies32; 
			if (qc -> min_rtt_us < qc-> prop_rtt_us)
				qc -> prop_rtt_us = qc -> min_rtt_us; 
		}
	}

	if(update_filter_expired && qc -> mode == NOTHING){ 
		qc -> mode = ESTIMATE_MIN_RTT; 
		qc -> last_probertt_stamp = tcp_jiffies32; 
		qc -> prior_cwnd = tp -> snd_cwnd;
		tp -> snd_cwnd = min(tp -> snd_cwnd, estimate_min_rtt_cwnd);
		qc -> min_rtt_us = rs -> rtt_us;
	}

	if(qc -> mode == ESTIMATE_MIN_RTT){
		estimate_rtt_expired = after(tcp_jiffies32, 
				qc -> last_probertt_stamp + msecs_to_jiffies(max_probertt_duration_msecs)); 
		if(estimate_rtt_expired){
			qc -> mode = NOTHING; 
			tp -> snd_cwnd = qc -> prior_cwnd;
		}
	}
}

static void q_cong_main(struct sock *sk, const struct rate_sample *rs){
	// struct tcp_sock *tp = tcp_sk(sk);

	training(sk, rs);
	update_min_rtt(sk,rs);
}



static void init_Q_cong(struct sock *sk){
	struct Q_cong *qc;
	struct tcp_sock *tp = tcp_sk(sk);
	u8 Q_row[numOfState] = {state0_max};
	u8 Q_col = numOfAction; 

	qc = inet_csk_ca(sk);

	qc -> mode = NOTHING;
	qc -> last_sequence = 0;
	qc -> estimated_throughput = 0;
	qc -> pre_throughput = 0;
	qc -> last_update_stamp = tcp_jiffies32;
	qc -> last_packet_loss = 0;

	qc -> last_probertt_stamp = tcp_jiffies32;
	qc -> min_rtt_us = tcp_min_rtt(tp);
	qc -> prop_rtt_us = tcp_min_rtt(tp);
	qc -> prior_cwnd = 0;
	qc -> retransmit_during_interval = 0;

	qc -> action = -1; 
	qc -> exited = 0; 
	qc -> prev_state[0] = 0;
	qc -> current_state[0] = 0;

	createMatrix(&matrix, Q_row, Q_col);
}

static void release_Q_cong(struct sock* sk){
	eraseMatrix(&matrix);
}

struct tcp_congestion_ops q_cong = {
	.flags		= TCP_CONG_NON_RESTRICTED, 
	.init		= init_Q_cong,
	.release	= release_Q_cong,
	.name 		= "tcpql",
	.owner		= THIS_MODULE,
	.ssthresh	= q_cong_ssthresh,
	.cong_control	= q_cong_main,
	.undo_cwnd 	= q_cong_undo_cwnd,
};

static int __init Q_cong_init(void){
	BUILD_BUG_ON(sizeof(struct Q_cong) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&q_cong);
}

static void __exit Q_cong_exit(void){
	tcp_unregister_congestion_control(&q_cong);
}

module_init(Q_cong_init);
module_exit(Q_cong_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HuiQi,JinyaoLiu");
MODULE_DESCRIPTION("tcpql : Learning based Congestion Control Algorithm");
