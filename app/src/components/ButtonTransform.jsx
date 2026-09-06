import { useState } from 'react'
import { motion } from 'motion/react'

export default function ButtonTransform({ label = 'happy' }) {
  const [big, setBig] = useState(false)

  return (
    <motion.button
      type="button"
      onClick={() => setBig(!big)}
      initial={false}
      animate={{ width: big ? 160 : 80, height: big ? 380 : 38 }}
      transition={{ type: 'spring', stiffness: 300, damping: 60 }}
      style={{
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        border: 'none',
        borderRadius: 12,
        background: '#fff',
        color: '#000',
        fontSize: 14,
        fontWeight: 600,
        cursor: 'pointer',
        overflow: 'hidden',
      }}
    >
      {label}
    </motion.button>
  )
}