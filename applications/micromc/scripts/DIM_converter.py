import os
import sys
import json
import struct
import numpy as np
from tqdm import tqdm
import nbtlib
from zlib import decompress
from collections import defaultdict
import minecraft_data

def read_mca_file(mca_file_path):
    """
    读取MCA格式文件，提取所有区块数据
    
    Args:
        mca_file_path: MCA文件路径
        
    Returns:
        dict: 以(x,z)为键的区块数据字典
    """
    chunks = {}
    
    print(f"开始读取MCA文件: {mca_file_path}")
    try:
        with open(mca_file_path, 'rb') as f:
            # MCA文件头包含1024个4字节整数，表示每个区块的位置和大小
            locations = []
            for _ in range(1024):
                offset_sectors, size_sectors = struct.unpack('>IB', f.read(4) + b'\0')
                offset = offset_sectors >> 8
                size = offset_sectors & 0xFF
                if offset > 0 and size > 0:  # 如果区块存在
                    locations.append((offset, size))
                else:
                    locations.append(None)
            
            # 读取每个区块
            for i, loc in enumerate(locations):
                if loc is None:
                    continue
                    
                offset, size = loc
                # 计算区块坐标
                chunk_x = i % 32
                chunk_z = i // 32
                
                # 跳转到区块数据位置
                f.seek(offset * 4096)  # 每个扇区4KB
                
                # 读取区块头
                length = struct.unpack('>I', f.read(4))[0]
                compression_type = struct.unpack('B', f.read(1))[0]
                
                # 读取压缩数据
                compressed_data = f.read(length - 1)
                
                # 根据压缩类型解压数据
                if compression_type == 1:  # GZIP
                    import gzip
                    from io import BytesIO
                    chunk_data = gzip.GzipFile(fileobj=BytesIO(compressed_data)).read()
                elif compression_type == 2:  # ZLIB
                    chunk_data = decompress(compressed_data)
                else:
                    print(f"未知的压缩类型 {compression_type}")
                    continue
                
                # 解析NBT数据
                try:
                    # 使用BytesIO创建一个文件对象
                    from io import BytesIO
                    chunk_io = BytesIO(chunk_data)
                    
                    # 使用nbtlib解析NBT数据
                    nbt_file = nbtlib.nbt.File.parse(chunk_io, byteorder='big')
                        
                    # 将区块数据保存到字典中
                    chunks[(chunk_x, chunk_z)] = nbt_file
                except Exception as e:
                    print(f"解析NBT数据时出错 (区块 {chunk_x},{chunk_z}): {e}")
    except Exception as e:
        print(f"读取MCA文件时出错: {e}")
        
    return chunks

def extract_block_ids(nbt_data):
    """
    从区块的NBT数据中提取方块ID
    
    Args:
        nbt_data: 区块的NBT数据
        
    Returns:
        list: 包含方块坐标和ID的列表
    """
    blocks_data = []
    
    try:
        # 获取区块坐标
        chunk_x = nbt_data["xPos"]
        chunk_z = nbt_data["zPos"]
        
        # 处理每个区段(Section)
        for section in nbt_data["sections"]:
            if "block_states" not in section:
                continue
                
            y_base = section["Y"] * 16
            block_states = section["block_states"]
            pallete = block_states["palette"]
            data = block_states["data"] if "data" in block_states else None
            if not data: continue
            # 按照pallete entry数量决定每个方块用多少位表示 (最小为4)
            bits_per_block = 4 if len(pallete) <= 16 else (len(pallete) - 1).bit_length()
            blocks_per_int64 = 64 // bits_per_block
            # 解压数据
            for i in range(len(data)):
                current_int64 = data[i]
                for j in range(blocks_per_int64):
                    index = i * blocks_per_int64 + j
                    if index >= 4096: break
                    block_pallete_id = (current_int64 >> (j * bits_per_block)) & ((1 << bits_per_block) - 1)
                    block_id = pallete[block_pallete_id]['Name']
                    
                    # 跳过空气方块（ID为0）
                    if block_id == 'minecraft:air':
                        continue

                    x = index % 16
                    z = (index // 16) % 16
                    y = index // 256
                        
                    # 计算全局坐标
                    global_y = y_base + y
                    
                    # 添加方块数据
                    block_data = {
                        "x": x,
                        "y": global_y,
                        "z": z,
                        "id": block_id
                    }
                        
                    blocks_data.append(block_data)
    except Exception as e:
        print(f"解析区块数据时出错: {e}")
        
    return blocks_data


def main():
    if len(sys.argv) < 2:
        # 如果没有指定参数，使用当前目录下的MCA文件
        mca_files = [f for f in os.listdir('.') if f.endswith('.mca')]
        if not mca_files:
            print("当前目录中没有MCA文件")
            print("用法: python DIM_converter.py <MCA文件路径或目录> [输出文件]")
            return
        
        input_path = '.'
        output_file = "minecraft_blocks.json"
    else:
        input_path = sys.argv[1]
        output_file = sys.argv[2] if len(sys.argv) > 2 else "minecraft_blocks.json"
    
    # 检查输入路径是文件还是目录
    if os.path.isfile(input_path):
        if not input_path.endswith('.mca'):
            print(f"{input_path} 不是一个MCA文件")
            return
            
        # 处理单个MCA文件
        chunks = read_mca_file(input_path)
        
        all_chunks = []
        max_num_chunks = 12
        num_chunks = 0
        block_count = 0
        for coords, chunk_data in chunks.items():
            chunk_blocks = extract_block_ids(chunk_data)
            block_count += len(chunk_blocks)
            all_chunks.append({
                "coords": coords,
                "blocks": chunk_blocks
            })
            num_chunks += 1
            if num_chunks >= max_num_chunks:
                print(f"已处理 {num_chunks} 个区块，停止处理")
                break
            
        result = {
            "chunks": all_chunks
        }
        
        # 保存为JSON
        with open(output_file, 'w') as f:
            json.dump(result, f)
            
        print(f"处理了 {len(all_chunks)} 个区块，提取了 {block_count} 个方块")
        print(f"数据已保存到 {output_file}")
    else: print("Specify a mca file please.")

if __name__ == "__main__":
    main()