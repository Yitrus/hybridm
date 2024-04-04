#include <linux/htmm.h>

//这个Q-Table的横纵坐标
#define NUM_STATES 10
#define NUM_ACTIONS 10

unsigned int suj_action=8; //nr_action是全局变量，每次更新完状态，通过这个桥梁更新action

// 显式初始化Q-Table
unsigned int Q[NUM_STATES][NUM_ACTIONS] = {
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10}, // 状态0的行动Q值
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10}, // 状态1的行动Q值
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10},
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10},
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10},
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10},
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10},
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10},
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10}, // 状态8的行动Q值
    {10, 20, 10, 20, 10, 6, 7, 8, 9, 10},  // 状态9的行动Q值
};

void get_best_action(unsigned int *nr_action){
	*nr_action = suj_action;
	return;
}

// 假设的状态到索引的映射函数
int map_state_to_index(unsigned long long A[], unsigned long long B[]) {
    // 这里需要实现具体的映射逻辑
    // 例如，基于A和B的某些特性或值来决定它们对应的状态索引
    // 此处只是一个示意，实际逻辑需要根据具体的状态定义来设计
    int state = 2;
    return state; // 返回一个示例索引
}

// 函数以A和B数组的组合形式接受当前状态，并返回最佳行动
void update_action(unsigned long long A[], unsigned long long B[]) {
    // 将状态映射为一个0到9之间的索引
    int state_index = map_state_to_index(A, B);
    
    // 为当前状态初始化最佳行动和最大Q值
    unsigned int best_action = 0;
    unsigned int max_q_value = Q[state_index][0];
    
    // 遍历当前状态下所有可能的行动，找到最大的Q值对应的行动
    int action; 
    for (action = 1; action < NUM_ACTIONS; action++) {
        if (Q[state_index][action] > max_q_value) {
            max_q_value = Q[state_index][action];
            best_action = action;
        }
    }
    
    suj_action = best_action;
	
    return;
}
